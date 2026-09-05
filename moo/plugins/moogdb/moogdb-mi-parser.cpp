/*
 *   moogdb-mi-parser.cpp
 *
 *   Implementation of the GDB/MI line parser.  The grammar (paraphrased
 *   from the GDB manual):
 *
 *     output     -> ( exec-async | status-async | notify-async
 *                   | console-stream | target-stream | log-stream )
 *                   *  ( result-record )?  "(gdb)"
 *     result-record    -> token? "^" result-class ( "," result )*
 *     exec-async       -> token? "*" async-class  ( "," result )*
 *     status-async     -> token? "+" async-class  ( "," result )*
 *     notify-async     -> token? "=" async-class  ( "," result )*
 *     console-stream   -> "~" c-string
 *     target-stream    -> "@" c-string
 *     log-stream       -> "&" c-string
 *     result           -> variable "=" value
 *     value            -> c-string | tuple | list
 *     tuple            -> "{}" | "{" result ( "," result )* "}"
 *     list             -> "[]" | "[" value ( "," value )* "]"
 *                       | "[" result ( "," result )* "]"
 *     c-string         -> '"' (escaped-chars)* '"'
 *
 *   We parse it with a hand-written recursive-descent walker that
 *   keeps the current pointer in a small ParseState and advances on
 *   each token.  No back-tracking is required.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#include "plugins/moogdb/moogdb-mi-parser.h"

#include <stdlib.h>
#include <string.h>

/* ── Value representation ─────────────────────────────────────────── */

typedef enum {
    VAL_STRING,
    VAL_TUPLE,           /* { name=value, ... }  — order-preserved   */
    VAL_LIST             /* [ value, ... ]  OR  [ name=value, ... ]  */
} ValueKind;

struct _MooGdbMiValue {
    ValueKind kind;
    union {
        char       *str;          /* VAL_STRING                       */
        GHashTable *tuple_fields; /* VAL_TUPLE: char* -> MooGdbMiValue*
                                   * — owned; values free'd via fn   */
        GPtrArray  *list;         /* VAL_LIST: MooGdbMiValue*         */
    } u;
    /* When kind==VAL_TUPLE we also keep insertion order for
     * deterministic iteration. */
    GPtrArray *tuple_order;       /* of char* (borrowed from tuple_fields) */
};

static void moo_gdb_mi_value_free (MooGdbMiValue *v);

static void
free_value_cb (gpointer p)
{
    moo_gdb_mi_value_free ((MooGdbMiValue *) p);
}

static void
moo_gdb_mi_value_free (MooGdbMiValue *v)
{
    if (!v) return;
    switch (v->kind) {
    case VAL_STRING:
        g_free (v->u.str);
        break;
    case VAL_TUPLE:
        g_hash_table_destroy (v->u.tuple_fields);
        g_ptr_array_free (v->tuple_order, TRUE);
        break;
    case VAL_LIST:
        g_ptr_array_free (v->u.list, TRUE);
        break;
    }
    g_free (v);
}

/* ── Record representation ────────────────────────────────────────── */

struct _MooGdbMiRecord {
    MooGdbMiKind  kind;
    int           token;        /* -1 if absent */
    char         *klass;        /* result-class / async-class; NULL for streams */
    char         *text;         /* for CONSOLE/TARGET/LOG; NULL otherwise */
    /* Top-level results are stored as a tuple value, even though MI
     * syntactically lists them flat — same lookup, fewer code paths. */
    MooGdbMiValue *fields;
};

/* ── Parse state ──────────────────────────────────────────────────── */

typedef struct {
    const char *p;     /* current cursor */
    const char *end;   /* one past last char */
    int         depth; /* nesting depth, to bound recursion */
} ParseState;

/* gdb's own output never nests anywhere near this deeply; the limit exists so
 * that malformed or hostile output cannot recurse the stack to death. */
#define MI_MAX_DEPTH 64

static MooGdbMiValue *parse_value  (ParseState *ps);
static gboolean       parse_result (ParseState *ps, char **out_name,
                                    MooGdbMiValue **out_val);

static void
skip_ws (ParseState *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t'))
        ps->p++;
}

static gboolean
peek (ParseState *ps, char c)
{
    return ps->p < ps->end && *ps->p == c;
}

static gboolean
eat (ParseState *ps, char c)
{
    if (peek (ps, c)) { ps->p++; return TRUE; }
    return FALSE;
}

/* Parse a C-string literal "..."; the leading `"` has already been
 * consumed.  De-escape \", \\, \n, \r, \t, \0NN octal, \xNN hex.
 * Returns a g_strdup'd payload (caller owns); or NULL on syntax err. */
static char *
parse_cstring_body (ParseState *ps)
{
    GString *out = g_string_new (NULL);
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\' && ps->p + 1 < ps->end) {
            ps->p++;
            switch (*ps->p) {
            case 'n': g_string_append_c (out, '\n'); ps->p++; break;
            case 'r': g_string_append_c (out, '\r'); ps->p++; break;
            case 't': g_string_append_c (out, '\t'); ps->p++; break;
            case '"': g_string_append_c (out, '"');  ps->p++; break;
            case '\\': g_string_append_c (out, '\\'); ps->p++; break;
            case '\'': g_string_append_c (out, '\''); ps->p++; break;
            case 'a': g_string_append_c (out, '\a'); ps->p++; break;
            case 'b': g_string_append_c (out, '\b'); ps->p++; break;
            case 'f': g_string_append_c (out, '\f'); ps->p++; break;
            case 'v': g_string_append_c (out, '\v'); ps->p++; break;
            default:
                /* Unknown escape — leave the backslash in. */
                g_string_append_c (out, '\\');
                g_string_append_c (out, *ps->p);
                ps->p++;
                break;
            }
        } else {
            g_string_append_c (out, *ps->p++);
        }
    }
    if (ps->p >= ps->end || *ps->p != '"') {
        g_string_free (out, TRUE);
        return NULL;
    }
    ps->p++;  /* eat closing `"` */
    return g_string_free (out, FALSE);
}

/* variable -> [a-zA-Z_][a-zA-Z0-9_-]*  */
static char *
parse_variable (ParseState *ps)
{
    const char *start = ps->p;
    if (ps->p >= ps->end) return NULL;
    char c = *ps->p;
    if (!(g_ascii_isalpha (c) || c == '_')) return NULL;
    ps->p++;
    while (ps->p < ps->end) {
        c = *ps->p;
        if (!(g_ascii_isalnum (c) || c == '_' || c == '-'))
            break;
        ps->p++;
    }
    return g_strndup (start, ps->p - start);
}

/* tuple -> '{}' | '{' result ( ',' result )* '}'  */
static MooGdbMiValue *
parse_tuple (ParseState *ps)
{
    if (!eat (ps, '{')) return NULL;
    MooGdbMiValue *v = g_new0 (MooGdbMiValue, 1);
    v->kind = VAL_TUPLE;
    v->u.tuple_fields = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, free_value_cb);
    v->tuple_order = g_ptr_array_new ();
    if (eat (ps, '}')) return v;

    for (;;) {
        char *name = NULL;
        MooGdbMiValue *child = NULL;
        if (!parse_result (ps, &name, &child)) {
            g_free (name);
            moo_gdb_mi_value_free (v);
            return NULL;
        }
        /* Take ownership of name+child by storing in the hash table.  On a
           duplicate key g_hash_table_insert frees the key we just passed and
           keeps the existing one, so the order array must not record it
           again. */
        gboolean is_new = !g_hash_table_contains (v->u.tuple_fields, name);
        g_hash_table_insert (v->u.tuple_fields, name, child);
        if (is_new)
            g_ptr_array_add (v->tuple_order, name);
        if (!eat (ps, ',')) break;
    }
    if (!eat (ps, '}')) {
        moo_gdb_mi_value_free (v);
        return NULL;
    }
    return v;
}

/* list -> '[]'
 *       | '[' value ( ',' value )* ']'
 *       | '[' result ( ',' result )* ']'
 * Distinguish by lookahead: if the first thing after '[' is a
 * variable name followed by `=`, it's a name-value list; otherwise
 * it's a value list.  We store both as VAL_LIST and just discard
 * the names — Phase 1 callers don't need them, and accessors return
 * the value side. */
static MooGdbMiValue *
parse_list (ParseState *ps)
{
    if (!eat (ps, '[')) return NULL;
    MooGdbMiValue *v = g_new0 (MooGdbMiValue, 1);
    v->kind = VAL_LIST;
    v->u.list = g_ptr_array_new_with_free_func (free_value_cb);
    if (eat (ps, ']')) return v;

    for (;;) {
        /* Lookahead: is this a `name=value` or a bare value? */
        const char *save = ps->p;
        char *name = parse_variable (ps);
        if (name && eat (ps, '=')) {
            MooGdbMiValue *child = parse_value (ps);
            g_free (name);
            if (!child) { moo_gdb_mi_value_free (v); return NULL; }
            g_ptr_array_add (v->u.list, child);
        } else {
            g_free (name);
            ps->p = save;
            MooGdbMiValue *child = parse_value (ps);
            if (!child) { moo_gdb_mi_value_free (v); return NULL; }
            g_ptr_array_add (v->u.list, child);
        }
        if (!eat (ps, ',')) break;
    }
    if (!eat (ps, ']')) {
        moo_gdb_mi_value_free (v);
        return NULL;
    }
    return v;
}

static MooGdbMiValue *
parse_value (ParseState *ps)
{
    skip_ws (ps);
    if (peek (ps, '"')) {
        ps->p++;
        char *s = parse_cstring_body (ps);
        if (!s) return NULL;
        MooGdbMiValue *v = g_new0 (MooGdbMiValue, 1);
        v->kind = VAL_STRING;
        v->u.str = s;
        return v;
    }
    if (peek (ps, '{') || peek (ps, '[')) {
        /* Bounded here rather than inside parse_tuple/parse_list, which have
           several return paths each; this is the only way into them. */
        if (ps->depth >= MI_MAX_DEPTH)
            return NULL;
        ps->depth++;
        MooGdbMiValue *v = peek (ps, '{') ? parse_tuple (ps) : parse_list (ps);
        ps->depth--;
        return v;
    }
    return NULL;
}

/* result -> variable '=' value */
static gboolean
parse_result (ParseState *ps, char **out_name, MooGdbMiValue **out_val)
{
    skip_ws (ps);
    char *name = parse_variable (ps);
    if (!name) return FALSE;
    if (!eat (ps, '=')) { g_free (name); return FALSE; }
    MooGdbMiValue *val = parse_value (ps);
    if (!val) { g_free (name); return FALSE; }
    *out_name = name;
    *out_val  = val;
    return TRUE;
}

/* ── Top-level: parse one MI line ─────────────────────────────────── */

MooGdbMiRecord *
moo_gdb_mi_parse (const char *line)
{
    if (!line || !*line) return NULL;
    size_t len = strlen (line);

    ParseState ps = { line, line + len, 0 };

    /* Optional token: leading digits before the type marker. */
    int  token   = -1;
    const char *p = ps.p;
    if (g_ascii_isdigit (*p)) {
        token = 0;
        while (p < ps.end && g_ascii_isdigit (*p)) {
            if (token > (G_MAXINT - 9) / 10)
                token = G_MAXINT;   /* saturate: never matches a pending id */
            else
                token = token * 10 + (*p - '0');
            p++;
        }
        ps.p = p;
    }

    if (ps.p >= ps.end) return NULL;

    MooGdbMiKind kind;
    char prefix = *ps.p;
    switch (prefix) {
    case '^': kind = MOO_GDB_MI_RESULT;  break;
    case '*': kind = MOO_GDB_MI_EXEC;    break;
    case '+': kind = MOO_GDB_MI_STATUS;  break;
    case '=': kind = MOO_GDB_MI_NOTIFY;  break;
    case '~': kind = MOO_GDB_MI_CONSOLE; break;
    case '@': kind = MOO_GDB_MI_TARGET;  break;
    case '&': kind = MOO_GDB_MI_LOG;     break;
    case '(':
        /* "(gdb)" prompt */
        if (g_str_has_prefix (ps.p, "(gdb)")) {
            MooGdbMiRecord *r = g_new0 (MooGdbMiRecord, 1);
            r->kind  = MOO_GDB_MI_PROMPT;
            r->token = -1;
            return r;
        }
        return NULL;
    default:
        return NULL;
    }
    ps.p++;

    MooGdbMiRecord *r = g_new0 (MooGdbMiRecord, 1);
    r->kind  = kind;
    r->token = token;

    if (kind == MOO_GDB_MI_CONSOLE
        || kind == MOO_GDB_MI_TARGET
        || kind == MOO_GDB_MI_LOG)
    {
        /* Stream records: just a C-string follows. */
        if (!eat (&ps, '"')) {
            moo_gdb_mi_record_free (r);
            return NULL;
        }
        r->text = parse_cstring_body (&ps);
        if (!r->text) {
            moo_gdb_mi_record_free (r);
            return NULL;
        }
        return r;
    }

    /* RESULT/EXEC/STATUS/NOTIFY: class identifier follows, then
     * an optional comma-separated result list. */
    r->klass = parse_variable (&ps);
    if (!r->klass) {
        moo_gdb_mi_record_free (r);
        return NULL;
    }

    if (eat (&ps, ',')) {
        /* Wrap the top-level results in a synthetic tuple value so
         * field lookup uses the same code path as nested tuples. */
        MooGdbMiValue *tup = g_new0 (MooGdbMiValue, 1);
        tup->kind = VAL_TUPLE;
        tup->u.tuple_fields = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, free_value_cb);
        tup->tuple_order = g_ptr_array_new ();

        for (;;) {
            char *name = NULL;
            MooGdbMiValue *child = NULL;
            if (!parse_result (&ps, &name, &child)) {
                g_free (name);
                moo_gdb_mi_value_free (tup);
                moo_gdb_mi_record_free (r);
                return NULL;
            }
            gboolean is_new =
                !g_hash_table_contains (tup->u.tuple_fields, name);
            g_hash_table_insert (tup->u.tuple_fields, name, child);
            if (is_new)
                g_ptr_array_add (tup->tuple_order, name);
            if (!eat (&ps, ',')) break;
        }
        r->fields = tup;
    }
    return r;
}

void
moo_gdb_mi_record_free (MooGdbMiRecord *r)
{
    if (!r) return;
    g_free (r->klass);
    g_free (r->text);
    if (r->fields)
        moo_gdb_mi_value_free (r->fields);
    g_free (r);
}

/* ── Accessors ────────────────────────────────────────────────────── */

MooGdbMiKind moo_gdb_mi_record_kind  (MooGdbMiRecord *r) { return r->kind;  }
int          moo_gdb_mi_record_token (MooGdbMiRecord *r) { return r->token; }
const char  *moo_gdb_mi_record_class (MooGdbMiRecord *r) { return r->klass; }
const char  *moo_gdb_mi_record_text  (MooGdbMiRecord *r) { return r->text;  }

MooGdbMiValue *
moo_gdb_mi_record_field (MooGdbMiRecord *r, const char *name)
{
    if (!r || !r->fields || r->fields->kind != VAL_TUPLE) return NULL;
    return (MooGdbMiValue *) g_hash_table_lookup (
        r->fields->u.tuple_fields, name);
}

const char *
moo_gdb_mi_value_string (MooGdbMiValue *v)
{
    return (v && v->kind == VAL_STRING) ? v->u.str : NULL;
}

MooGdbMiValue *
moo_gdb_mi_value_tuple_get (MooGdbMiValue *v, const char *name)
{
    if (!v || v->kind != VAL_TUPLE) return NULL;
    return (MooGdbMiValue *) g_hash_table_lookup (v->u.tuple_fields, name);
}

guint
moo_gdb_mi_value_list_len (MooGdbMiValue *v)
{
    return (v && v->kind == VAL_LIST) ? v->u.list->len : 0;
}

MooGdbMiValue *
moo_gdb_mi_value_list_nth (MooGdbMiValue *v, guint i)
{
    if (!v || v->kind != VAL_LIST || i >= v->u.list->len) return NULL;
    return (MooGdbMiValue *) g_ptr_array_index (v->u.list, i);
}
