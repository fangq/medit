/*
 *   moogdb-project.cpp
 *
 *   Project discovery + launch.json/tasks.json parsing.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#include "plugins/moogdb/moogdb-project.h"
#include "plugins/moogdb/moogdb-json.h"
#include <string.h>
#include <gio/gio.h>

struct _MooGdbProject {
    char       *root;          /* abs path containing .medit/ or .vscode/ */
    char       *config_dir;    /* "<root>/.medit" or "<root>/.vscode"    */
    char       *launch_path;   /* absolute path of launch.json            */
    GPtrArray  *configs;       /* MooGdbConfig* (free-func attached)      */
    GHashTable *tasks;         /* char*label → char*command                */
};

/* Subdirectories searched, in priority order — `.medit` wins so a
 * medit-tuned launch.json can shadow a project's existing VS Code
 * one without removing the latter. */
static const char *const PROJECT_DIRS[] = { ".medit", ".vscode", NULL };

static void
free_config (gpointer p)
{
    MooGdbConfig *c = (MooGdbConfig *) p;
    if (!c) return;
    g_free (c->name);
    g_free (c->program);
    g_strfreev (c->args);
    g_free (c->cwd);
    if (c->environment) g_hash_table_destroy (c->environment);
    g_free (c->pre_launch_task);
    g_free (c->build_command);
    g_free (c);
}

/* ── Discovery: walk up looking for launch.json ─────────────────────
 *
 * Two candidate config directories per level — `.medit` first so a
 * medit-specific launch.json shadows any VS Code one in the same
 * project.  Returns the project root in `*out_root` and the chosen
 * config-dir name in `*out_dir` (".medit" / ".vscode"). */
static gboolean
find_project_root (const char *start_path,
                    char **out_root, char **out_dir)
{
    *out_root = *out_dir = NULL;
    if (!start_path || !*start_path) return FALSE;

    char *dir = NULL;
    if (g_file_test (start_path, G_FILE_TEST_IS_DIR))
        dir = g_strdup (start_path);
    else
        dir = g_path_get_dirname (start_path);

    while (dir && *dir) {
        for (int i = 0; PROJECT_DIRS[i]; i++) {
            char *candidate = g_build_filename (dir, PROJECT_DIRS[i],
                                                  "launch.json", (const char*) NULL);
            if (g_file_test (candidate, G_FILE_TEST_IS_REGULAR)) {
                g_free (candidate);
                *out_root = dir;
                *out_dir  = g_strdup (PROJECT_DIRS[i]);
                return TRUE;
            }
            g_free (candidate);
        }

        /* Step one level up.  When g_path_get_dirname is given "/"
         * it returns "/" — break the loop in that case. */
        char *parent = g_path_get_dirname (dir);
        if (!strcmp (parent, dir)) {
            g_free (parent);
            break;
        }
        g_free (dir);
        dir = parent;
    }
    g_free (dir);
    return FALSE;
}

/* ── tasks.json parsing ──────────────────────────────────────────── */

/* Build the label→command map from .vscode/tasks.json (if present).
 * Each task can be either:
 *
 *   { "label": "build", "type": "shell", "command": "make" }
 *   { "label": "build", "type": "shell", "command": "make",
 *     "args": ["-j8", "all"] }
 *
 * We handle both — joining `args` onto `command` with shell-style
 * quoting in the latter case so a downstream g_shell_parse_argv
 * survives spaces in arguments. */
static char *
task_command_from_node (MooJsonValue *task)
{
    const char *cmd = moo_json_object_string (task, "command", NULL);
    if (!cmd) return NULL;

    MooJsonValue *args = moo_json_object_get (task, "args");
    if (!args || moo_json_kind (args) != MOO_JSON_ARRAY ||
        moo_json_array_len (args) == 0)
        return g_strdup (cmd);

    GString *out = g_string_new (NULL);
    char *qcmd = g_shell_quote (cmd);
    g_string_append (out, qcmd);
    g_free (qcmd);
    guint n = moo_json_array_len (args);
    for (guint i = 0; i < n; i++) {
        const char *a =
            moo_json_string (moo_json_array_get (args, i));
        if (!a) continue;
        g_string_append_c (out, ' ');
        char *q = g_shell_quote (a);
        g_string_append (out, q);
        g_free (q);
    }
    return g_string_free (out, FALSE);
}

static GHashTable *
load_tasks (const char *root, const char *config_dir)
{
    GHashTable *map = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, g_free);
    char *path = g_build_filename (root, config_dir, "tasks.json", (const char*) NULL);
    char *body = NULL;
    gsize len  = 0;
    if (!g_file_get_contents (path, &body, &len, NULL)) {
        g_free (path);
        return map;   /* tasks.json optional */
    }
    g_free (path);

    GError *err = NULL;
    MooJsonValue *root_v = moo_json_parse (body, &err);
    g_free (body);
    if (!root_v) {
        g_warning ("[gdb] failed to parse tasks.json: %s",
                    err ? err->message : "(unknown)");
        if (err) g_error_free (err);
        return map;
    }
    MooJsonValue *tasks = moo_json_object_get (root_v, "tasks");
    if (tasks && moo_json_kind (tasks) == MOO_JSON_ARRAY) {
        guint n = moo_json_array_len (tasks);
        for (guint i = 0; i < n; i++) {
            MooJsonValue *t = moo_json_array_get (tasks, i);
            const char *label = moo_json_object_string (t, "label", NULL);
            char *cmd = task_command_from_node (t);
            if (label && cmd)
                g_hash_table_insert (map, g_strdup (label), cmd);
            else
                g_free (cmd);
        }
    }
    moo_json_value_free (root_v);
    return map;
}

/* ── launch.json: per-config conversion ──────────────────────────── */

/* Heuristic filter: keep a config if it looks like something gdb
 * can drive.  VS Code's standard cppdbg uses type=cppdbg; gdb-via-
 * native-debug uses type=gdb.  We also accept configs that lack a
 * type entirely (a hand-written launch.json without VS Code-isms). */
static gboolean
config_is_gdb_compatible (MooJsonValue *cfg)
{
    const char *type = moo_json_object_string (cfg, "type", NULL);
    if (type) {
        if (!strcmp (type, "cppdbg"))    return TRUE;
        if (!strcmp (type, "gdb"))       return TRUE;
        if (!strcmp (type, "cppvsdbg"))  return TRUE;
        if (!strcmp (type, "by-gdb"))    return TRUE;
        return FALSE;
    }
    /* No type → accept it; user knows what they're doing. */
    return TRUE;
}

static MooGdbConfig *
config_from_node (MooJsonValue *node, GHashTable *tasks)
{
    if (!node || moo_json_kind (node) != MOO_JSON_OBJECT) return NULL;
    if (!config_is_gdb_compatible (node)) return NULL;

    MooGdbConfig *c = g_new0 (MooGdbConfig, 1);
    c->name    = g_strdup (moo_json_object_string (node, "name",
                                                     "(unnamed)"));
    /* program is VS Code's spelling; accept "target" too for medit
     * native shape. */
    c->program = g_strdup (moo_json_object_string (node, "program",
                            moo_json_object_string (node, "target", "")));
    c->cwd     = g_strdup (moo_json_object_string (node, "cwd", ""));

    MooJsonValue *args = moo_json_object_get (node, "args");
    if (args && moo_json_kind (args) == MOO_JSON_ARRAY) {
        guint n = moo_json_array_len (args);
        c->args = g_new0 (char *, n + 1);
        for (guint i = 0; i < n; i++) {
            const char *s =
                moo_json_string (moo_json_array_get (args, i));
            c->args[i] = g_strdup (s ? s : "");
        }
    }

    /* VS Code: environment is an array of {name,value} pairs.
     * medit-native shorthand: a plain object.  Accept both. */
    c->environment = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, g_free);
    MooJsonValue *env = moo_json_object_get (node, "environment");
    if (env && moo_json_kind (env) == MOO_JSON_ARRAY) {
        guint n = moo_json_array_len (env);
        for (guint i = 0; i < n; i++) {
            MooJsonValue *e = moo_json_array_get (env, i);
            const char *name  = moo_json_object_string (e, "name",  NULL);
            const char *value = moo_json_object_string (e, "value", "");
            if (name)
                g_hash_table_insert (c->environment,
                    g_strdup (name), g_strdup (value));
        }
    }
    /* else: env left empty if not present or wrong shape. */

    const char *task = moo_json_object_string (node, "preLaunchTask", NULL);
    if (task) c->pre_launch_task = g_strdup (task);

    if (c->pre_launch_task && tasks) {
        const char *cmd = (const char *)
            g_hash_table_lookup (tasks, c->pre_launch_task);
        if (cmd) c->build_command = g_strdup (cmd);
    }
    /* Fallback: medit-native shape with `"build": "make ..."`. */
    if (!c->build_command) {
        const char *b = moo_json_object_string (node, "build", NULL);
        if (b) c->build_command = g_strdup (b);
    }

    return c;
}

/* ── Public API ──────────────────────────────────────────────────── */

MooGdbProject *
moo_gdb_project_load (const char *start_path, GError **error)
{
    char *root = NULL, *config_dir = NULL;
    if (!find_project_root (start_path, &root, &config_dir)) {
        g_set_error_literal (error,
            g_quark_from_static_string ("moo-gdb-project"), 0,
            "no .medit/launch.json or .vscode/launch.json found "
            "in this or any parent directory");
        return NULL;
    }
    char *launch_path = g_build_filename (root, config_dir,
                                            "launch.json", (const char*) NULL);
    char *body = NULL;
    gsize len  = 0;
    if (!g_file_get_contents (launch_path, &body, &len, error)) {
        g_free (root);
        g_free (config_dir);
        g_free (launch_path);
        return NULL;
    }
    MooJsonValue *jroot = moo_json_parse (body, error);
    g_free (body);
    if (!jroot) {
        g_free (root);
        g_free (config_dir);
        g_free (launch_path);
        return NULL;
    }

    GHashTable *tasks = load_tasks (root, config_dir);

    MooGdbProject *p = g_new0 (MooGdbProject, 1);
    p->root        = root;
    p->config_dir  = config_dir;
    p->launch_path = launch_path;
    p->configs     = g_ptr_array_new_with_free_func (free_config);
    p->tasks       = tasks;

    MooJsonValue *cfgs = moo_json_object_get (jroot, "configurations");
    if (cfgs && moo_json_kind (cfgs) == MOO_JSON_ARRAY) {
        guint n = moo_json_array_len (cfgs);
        for (guint i = 0; i < n; i++) {
            MooGdbConfig *c =
                config_from_node (moo_json_array_get (cfgs, i), tasks);
            if (c) g_ptr_array_add (p->configs, c);
        }
    }
    moo_json_value_free (jroot);
    return p;
}

void
moo_gdb_project_free (MooGdbProject *p)
{
    if (!p) return;
    g_free (p->root);
    g_free (p->config_dir);
    g_free (p->launch_path);
    if (p->configs) g_ptr_array_free (p->configs, TRUE);
    if (p->tasks)   g_hash_table_destroy (p->tasks);
    g_free (p);
}

const char *
moo_gdb_project_root (MooGdbProject *p) { return p ? p->root : NULL; }

const char *
moo_gdb_project_launch_path (MooGdbProject *p)
    { return p ? p->launch_path : NULL; }

guint
moo_gdb_project_n_configs (MooGdbProject *p)
    { return p && p->configs ? p->configs->len : 0; }

const MooGdbConfig *
moo_gdb_project_config (MooGdbProject *p, guint i)
{
    if (!p || !p->configs || i >= p->configs->len) return NULL;
    return (const MooGdbConfig *) p->configs->pdata[i];
}

/* ── Variable substitution ───────────────────────────────────────── */

static gboolean
match_var (const char *s, const char *name)
{
    size_t n = strlen (name);
    if (strncmp (s, name, n) != 0) return FALSE;
    /* Variable name must be terminated by `}` or `:`. */
    return s[n] == '}' || s[n] == ':';
}

char *
moo_gdb_project_resolve (MooGdbProject *p, const char *raw,
                          const char *active_file)
{
    if (!raw) return NULL;
    GString *out = g_string_new (NULL);
    const char *cur = raw;
    while (*cur) {
        if (cur[0] != '$' || cur[1] != '{') {
            g_string_append_c (out, *cur);
            cur++;
            continue;
        }
        const char *end = strchr (cur, '}');
        if (!end) {
            /* Unterminated — leave literal so the user sees their
             * typo rather than silently swallowing the rest. */
            g_string_append (out, cur);
            break;
        }
        const char *body = cur + 2;
        size_t body_len = end - body;

        char *replacement = NULL;
        if (match_var (body, "workspaceFolder")) {
            replacement = g_strdup (p ? p->root : "");
        } else if (match_var (body, "file")) {
            replacement = g_strdup (active_file ? active_file : "");
        } else if (match_var (body, "fileBasename")) {
            replacement = active_file ?
                g_path_get_basename (active_file) : g_strdup ("");
        } else if (match_var (body, "fileBasenameNoExtension")) {
            if (active_file) {
                char *base = g_path_get_basename (active_file);
                char *dot  = strrchr (base, '.');
                if (dot && dot != base) *dot = '\0';
                replacement = base;
            } else replacement = g_strdup ("");
        } else if (match_var (body, "fileDirname")) {
            replacement = active_file ?
                g_path_get_dirname (active_file) : g_strdup ("");
        } else if (body_len > 4 && !strncmp (body, "env:", 4)) {
            char *var = g_strndup (body + 4, body_len - 4);
            const char *val = g_getenv (var);
            replacement = g_strdup (val ? val : "");
            g_free (var);
        } else {
            /* Unknown variable — preserve verbatim. */
            replacement = g_strndup (cur, end - cur + 1);
        }
        g_string_append (out, replacement);
        g_free (replacement);
        cur = end + 1;
    }
    return g_string_free (out, FALSE);
}
