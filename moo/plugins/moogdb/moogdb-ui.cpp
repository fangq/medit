/*
 *   moogdb-ui.cpp
 *
 *   Phase 1: just enough UI to verify the gdb subprocess + MI parser
 *   round-trip.  "Debug → Test GDB connection" pops up a dialog,
 *   spawns gdb, runs `-gdb-version`, accumulates the console-output
 *   lines into the dialog's body, and tears the session down when
 *   the user closes the dialog.  Subsequent commits replace this
 *   with breakpoint margins / toolbar / current-line marker.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#include "plugins/moogdb/moogdb-ui.h"
#include "plugins/moogdb/moogdb-session.h"
#include "plugins/moogdb/moogdb-project.h"
#include "mooglib/moo-stat.h"

#include "mooedit/mooeditor.h"
#include "mooedit/mooeditview.h"
#include "mooedit/mootextview.h"
#include "mooedit/mootextbuffer.h"
#include "mooedit/moolinemark.h"
#include "mooutils/mooi18n.h"
#include "mooutils/moopane.h"
#include <gdk/gdkkeysyms.h>
#include <string.h>

typedef struct {
    GtkWidget     *dialog;
    GtkTextBuffer *buffer;
    MooGdbSession *session;
} TestCtx;

static void
on_console_output (G_GNUC_UNUSED MooGdbSession *s,
                   const char *line, gpointer user_data)
{
    TestCtx *ctx = (TestCtx *) user_data;
    if (!line) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter (ctx->buffer, &end);
    gtk_text_buffer_insert (ctx->buffer, &end, line, -1);
}

static void
on_state_changed (MooGdbSession *s, gpointer user_data)
{
    TestCtx *ctx = (TestCtx *) user_data;
    MooGdbState st = moo_gdb_session_get_state (s);
    if (st == MOO_GDB_STATE_ERROR || st == MOO_GDB_STATE_EXITED) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter (ctx->buffer, &end);
        const char *msg = (st == MOO_GDB_STATE_ERROR)
            ? "\n[gdb session error]\n"
            : "\n[gdb session ended]\n";
        gtk_text_buffer_insert (ctx->buffer, &end, msg, -1);
    }
}

/* Cleanup lives on "destroy", not "response": the dialog is created with
 * GTK_DIALOG_DESTROY_WITH_PARENT, and closing the editor window destroys it
 * without ever emitting "response".  Hanging the teardown off "response"
 * meant the gdb subprocess was never killed and outlived medit itself. */
static void
on_dialog_destroy (G_GNUC_UNUSED GtkWidget *dlg, gpointer user_data)
{
    TestCtx *ctx = (TestCtx *) user_data;
    if (ctx->session) {
        g_signal_handlers_disconnect_by_data (ctx->session, ctx);
        moo_gdb_session_quit (ctx->session);
        moo_gdb_session_shutdown (ctx->session);
        g_object_unref (ctx->session);
        ctx->session = NULL;
    }
    g_free (ctx);
}

static void
on_dialog_response (GtkDialog *dlg,
                    G_GNUC_UNUSED int response,
                    G_GNUC_UNUSED gpointer user_data)
{
    gtk_widget_destroy (GTK_WIDGET (dlg));
}

void
moo_gdb_ui_test_connection (MooEditWindow *window)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons (
        _("GDB Connection Test"),
        GTK_WINDOW (window),
        (GtkDialogFlags) (GTK_DIALOG_DESTROY_WITH_PARENT),
        _("_Close"), GTK_RESPONSE_CLOSE,
        (const char *) NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 600, 380);

    GtkWidget *content =
        gtk_dialog_get_content_area (GTK_DIALOG (dialog));

    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand (scroll, TRUE);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtk_box_pack_start (GTK_BOX (content), scroll, TRUE, TRUE, 0);

    GtkWidget *view = gtk_text_view_new ();
    gtk_text_view_set_editable      (GTK_TEXT_VIEW (view), FALSE);
    gtk_text_view_set_monospace     (GTK_TEXT_VIEW (view), TRUE);
    gtk_text_view_set_left_margin   (GTK_TEXT_VIEW (view), 8);
    gtk_text_view_set_right_margin  (GTK_TEXT_VIEW (view), 8);
    gtk_container_add (GTK_CONTAINER (scroll), view);

    TestCtx *ctx = g_new0 (TestCtx, 1);
    ctx->dialog  = dialog;
    ctx->buffer  = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    ctx->session = moo_gdb_session_new ();

    /* Header line so the user can see something happened even if gdb
     * doesn't print any console lines. */
    gtk_text_buffer_set_text (ctx->buffer,
        "Spawning gdb (mi3) and running -gdb-version...\n\n", -1);

    g_signal_connect (ctx->session, "console-output",
                      G_CALLBACK (on_console_output), ctx);
    g_signal_connect (ctx->session, "state-changed",
                      G_CALLBACK (on_state_changed), ctx);
    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_dialog_response), ctx);
    g_signal_connect (dialog, "destroy",
                      G_CALLBACK (on_dialog_destroy), ctx);

    gtk_widget_show_all (dialog);

    GError *err = NULL;
    if (!moo_gdb_session_start (ctx->session, NULL, &err)) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter (ctx->buffer, &end);
        char *msg = g_strdup_printf (
            "Failed to spawn gdb: %s\n\n"
            "Make sure `gdb` is installed and on your PATH.\n",
            err ? err->message : "(unknown error)");
        gtk_text_buffer_insert (ctx->buffer, &end, msg, -1);
        g_free (msg);
        if (err) g_error_free (err);
    }
}

/* ═════════════ Per-window state + breakpoint margin ═════════════════
 *
 * MooGdbWin holds one MooGdbSession (lazily created on first use)
 * plus a `breakpoints` table keyed by canonical file path.  Each
 * value is a GHashTable<int line -> Breakpoint*>, where Breakpoint
 * carries the gdb-assigned number plus the MooLineMark that shows
 * the red dot in the gutter.
 */

typedef struct {
    int          number;       /* -1 until -break-insert reply arrives */
    char        *file;
    int          line;
    MooLineMark *mark;         /* visual mark in the gutter (or NULL) */
} GdbBreakpoint;

struct _MooGdbWin {
    MooEditWindow *window;
    MooGdbSession *session;        /* lazily created */
    GHashTable    *bp_by_file;     /* char* -> GHashTable<int, GdbBreakpoint*> */
    GHashTable    *bp_by_number;   /* int   -> GdbBreakpoint* (borrowed) */

    /* Current-line marker.  At most one across the whole window:
     * when the debugger stops, we open the relevant file and place
     * an arrow icon at the line; when it resumes (or quits), we
     * remove the mark.  exec_file/exec_line cache the location for
     * possible later use (doc-load re-attach) when we add that. */
    MooLineMark   *exec_mark;
    char          *exec_file;
    int            exec_line;

    /* Console pane: monospaced log of gdb's ~/&/@ stream records
     * plus error replies, with an entry below for the user to send
     * raw commands.  Lives in the editor window's bottom pane area
     * (MOO_PANE_POS_BOTTOM). */
    GtkWidget     *console_pane;
    GtkTextBuffer *console_buffer;
    GtkTextView   *console_view;
    GtkEntry      *console_entry;

    /* Per-session user-configurable settings, set via the
     * "Configure Target" dialog.  All optional — when unset, the
     * old extension-strip heuristic kicks in for the target and
     * gdb inherits the editor's cwd / empty args. */
    char          *cfg_target;
    char          *cfg_args;
    char          *cfg_cwd;

    /* Locals pane: a GtkTreeView showing the current frame's
     * variables.  Populated from moo_gdb_session_get_locals each
     * time the session emits "locals-changed".  Lives on the
     * right side, alongside any future stack / watch panels.
     * Backing store is a GtkTreeStore (not list) so we can lazily
     * drill into structs/arrays via gdb varobjs. */
    GtkWidget     *locals_pane;
    GtkTreeStore  *locals_store;
    GtkTreeView   *locals_view;

    /* Stack pane: a GtkTreeView showing the call frames.  Double-
     * click selects the frame via -stack-select-frame (and re-
     * pulls locals at that frame). */
    GtkWidget     *frames_pane;
    GtkListStore  *frames_store;

    /* Watches pane: user-typed expressions + their last-eval
     * results.  Entry below the tree-view for adding a new watch. */
    GtkWidget     *watches_pane;
    GtkListStore  *watches_store;
    GtkTreeView   *watches_view;
    GtkEntry      *watches_entry;

    /* Hover-to-inspect cache.  Maps C-identifier string → most-recent
     * formatted value (or error message prefixed with "!").  Cleared
     * on every *running / *exited so a stale value from an old frame
     * never lingers on screen.  hover_pending is a set of identifiers
     * for which a -data-evaluate-expression is in flight — used to
     * avoid spamming gdb with repeat requests while a tooltip is
     * still mid-query. */
    GHashTable    *hover_cache;     /* char* -> char* */
    GHashTable    *hover_pending;   /* char* -> dummy non-NULL */

    /* Views this window has hooked "query-tooltip" on.  attach_hover_to_open_docs
     * walks the global document list, so these outlive the window unless they
     * are disconnected explicitly in moo_gdb_win_free. */
    GPtrArray     *hover_views;     /* MooEditView*, unowned */

    /* Launch-config project.  NULL when no `.medit/launch.json` or
     * `.vscode/launch.json` is found upwards from the active doc.
     * `active_config` indexes into project->configs[]; -1 if no
     * project is loaded.  cfg_combo + label live in the toolbar so
     * we can refresh them when the project (re)loads. */
    MooGdbProject *project;
    int            active_config;
    GtkComboBoxText *cfg_combo;
    GtkLabel        *cfg_label;
    /* Signal handler id on the editor window's "notify::active-doc"
     * — kept so the destructor can detach cleanly. */
    gulong         active_doc_handler;
    /* Same idea for "new-doc": forces a project rescan every time
     * a fresh document is loaded into the window, so a freshly-
     * authored .medit/launch.json gets picked up without the user
     * having to first switch tabs. */
    gulong         new_doc_handler;

    /* Build task tracking.  is_building==TRUE while a preLaunchTask
     * subprocess is running; suppresses concurrent build / start
     * presses.  pending_start carries the post-build action to run
     * on success (NULL if the build was a manual one-shot). */
    gboolean       is_building;
    GCancellable  *build_cancel;
    /* In-flight build, if any.  Unowned: the BuildCtx outlives this struct
     * when the window is closed mid-build, and clears its own back-pointer. */
    struct _BuildCtx *build_ctx;
};

/* Forward declaration: console_append is defined in the console-pane
 * section further down but used by the watches pane's "no session"
 * error path above it. */
static void console_append (MooGdbWin *win, const char *text,
                            const char *tag_name);

/* ── Locals pane ─────────────────────────────────────────────────── */

/* Single combined right-side pane that stacks Locals / Stack /
 * Watches vertically.  The three sections are still owned/built in
 * isolation (build_*_section below), then mounted into a nested
 * GtkPaned in build_inspect_pane so the user can drag the dividers
 * to give a struct dump more room without losing sight of the
 * stack or watches. */
#define MOO_GDB_INSPECT_PANE_ID "MooGdbInspect"

/* Tree-store columns.  The visible ones come first; the rest are
 * bookkeeping needed to drive lazy expansion via gdb varobjs:
 *
 *   EXPR        — the full gdb expression for this row, used as the
 *                 argument to -var-create the first time the user
 *                 expands an aggregate.  For top-level locals this
 *                 is the variable's name; for child rows it's gdb's
 *                 own varobj name (since varobjs can be re-used as
 *                 expressions in further MI commands).
 *   VAROBJ      — the gdb-assigned varobj handle, populated after
 *                 -var-create returns.  Empty string until then.
 *   LOADED      — TRUE once we've replaced the placeholder child
 *                 with real children.  Prevents redundant -var-list-
 *                 children calls when the user collapses then re-
 *                 expands the same row inside a single stop.
 *   EXPANDABLE  — TRUE if this row should display an expansion
 *                 arrow.  GtkTreeView shows arrows whenever a row
 *                 has children, including the placeholder dummy
 *                 we insert for unloaded aggregates. */
enum {
    LOCALS_COL_NAME,
    LOCALS_COL_TYPE,
    LOCALS_COL_VALUE,
    LOCALS_COL_EXPR,
    LOCALS_COL_VAROBJ,
    LOCALS_COL_LOADED,
    LOCALS_COL_EXPANDABLE,
    LOCALS_N_COLS
};

/* Forward decls for the var-create / var-children callbacks. */
static void on_locals_var_create  (MooGdbSession *s, const char *expr,
                                    const char *varobj, const char *type,
                                    const char *value, int numchild,
                                    gboolean is_error, gpointer user_data);
static void on_locals_var_children (MooGdbSession *s, const char *parent,
                                     GPtrArray *children,
                                     gboolean is_error, gpointer user_data);

/* Per-row context shared across the async var-create / var-children
 * path.  GtkTreeRowReference auto-invalidates on store rebuild so we
 * can safely check before writing back. */
typedef struct {
    MooGdbWin           *win;
    GtkTreeRowReference *row;
} LocalsRowCtx;

static void
locals_row_ctx_free (LocalsRowCtx *ctx)
{
    if (!ctx) return;
    if (ctx->row) gtk_tree_row_reference_free (ctx->row);
    g_free (ctx);
}

/* Append a placeholder "loading…" child row under `parent` so the
 * expansion arrow appears.  We never display the placeholder text
 * directly to the user — the row gets removed and replaced by real
 * children on first expand. */
static void
locals_add_dummy_child (GtkTreeStore *store, GtkTreeIter *parent)
{
    GtkTreeIter dummy;
    gtk_tree_store_append (store, &dummy, parent);
    gtk_tree_store_set (store, &dummy,
        LOCALS_COL_NAME,       "",
        LOCALS_COL_TYPE,       "",
        LOCALS_COL_VALUE,      "...",
        LOCALS_COL_EXPR,       "",
        LOCALS_COL_VAROBJ,     "",
        LOCALS_COL_LOADED,     FALSE,
        LOCALS_COL_EXPANDABLE, FALSE,
        -1);
}

/* When the user expands a previously-unloaded row, kick off the
 * varobj round-trip.  Three states the row can be in:
 *
 *   1) Has no varobj yet  →  -var-create, then -var-list-children
 *   2) Has varobj, not loaded  →  -var-list-children
 *   3) Loaded  →  no-op (children are already in the store)
 */
static void
on_locals_row_expanded (G_GNUC_UNUSED GtkTreeView *tv,
                        GtkTreeIter *iter, GtkTreePath *path,
                        gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!win->session || !win->locals_store) return;

    gboolean loaded = FALSE;
    char *expr = NULL, *varobj = NULL;
    gtk_tree_model_get (GTK_TREE_MODEL (win->locals_store), iter,
        LOCALS_COL_LOADED, &loaded,
        LOCALS_COL_EXPR,   &expr,
        LOCALS_COL_VAROBJ, &varobj,
        -1);
    if (loaded) { g_free (expr); g_free (varobj); return; }
    if (!expr || !*expr) { g_free (expr); g_free (varobj); return; }

    LocalsRowCtx *ctx = g_new0 (LocalsRowCtx, 1);
    ctx->win = win;
    ctx->row = gtk_tree_row_reference_new (
        GTK_TREE_MODEL (win->locals_store), path);

    if (varobj && *varobj) {
        moo_gdb_session_var_children (win->session, varobj,
                                       on_locals_var_children, ctx);
    } else {
        moo_gdb_session_var_create (win->session, expr,
                                     on_locals_var_create, ctx);
    }
    g_free (expr);
    g_free (varobj);
}

/* var-create reply: store the varobj name on the row, then chain
 * into var-children to actually fetch the kids. */
static void
on_locals_var_create (MooGdbSession *s, G_GNUC_UNUSED const char *expr,
                      const char *varobj, G_GNUC_UNUSED const char *type,
                      const char *value, int numchild,
                      gboolean is_error, gpointer user_data)
{
    LocalsRowCtx *ctx = (LocalsRowCtx *) user_data;
    MooGdbWin *win = ctx->win;
    if (is_error || !varobj ||
        !gtk_tree_row_reference_valid (ctx->row))
    {
        if (is_error && value && gtk_tree_row_reference_valid (ctx->row)) {
            GtkTreePath *p = gtk_tree_row_reference_get_path (ctx->row);
            GtkTreeIter it;
            if (gtk_tree_model_get_iter (GTK_TREE_MODEL (win->locals_store),
                                          &it, p))
                gtk_tree_store_set (win->locals_store, &it,
                    LOCALS_COL_VALUE,  value,
                    LOCALS_COL_LOADED, TRUE,
                    -1);
            if (p) gtk_tree_path_free (p);
        }
        locals_row_ctx_free (ctx);
        return;
    }

    GtkTreePath *p = gtk_tree_row_reference_get_path (ctx->row);
    GtkTreeIter it;
    if (p && gtk_tree_model_get_iter (GTK_TREE_MODEL (win->locals_store),
                                       &it, p))
    {
        gtk_tree_store_set (win->locals_store, &it,
            LOCALS_COL_VAROBJ, varobj,
            -1);
        /* If gdb gave us a value (rare for true aggregates, but
         * happens for pointers), fill it in too. */
        if (value && *value)
            gtk_tree_store_set (win->locals_store, &it,
                LOCALS_COL_VALUE, value, -1);
    }
    if (p) gtk_tree_path_free (p);

    if (numchild > 0) {
        /* Chain into var-children with the same row-ref context. */
        moo_gdb_session_var_children (s, varobj,
                                       on_locals_var_children, ctx);
    } else {
        /* Genuine scalar (or empty struct) — nothing more to show.
         * Remove the dummy and mark loaded. */
        if (gtk_tree_row_reference_valid (ctx->row)) {
            GtkTreePath *p2 = gtk_tree_row_reference_get_path (ctx->row);
            GtkTreeIter parent_it, child_it;
            if (p2 && gtk_tree_model_get_iter (
                    GTK_TREE_MODEL (win->locals_store), &parent_it, p2))
            {
                while (gtk_tree_model_iter_children (
                        GTK_TREE_MODEL (win->locals_store),
                        &child_it, &parent_it))
                    gtk_tree_store_remove (win->locals_store, &child_it);
                gtk_tree_store_set (win->locals_store, &parent_it,
                    LOCALS_COL_LOADED,     TRUE,
                    LOCALS_COL_EXPANDABLE, FALSE,
                    -1);
            }
            if (p2) gtk_tree_path_free (p2);
        }
        locals_row_ctx_free (ctx);
    }
}

/* var-list-children reply: replace the placeholder dummy under the
 * parent row with real child rows.  Each child that has its own
 * numchild > 0 gets its own dummy placeholder so the user can keep
 * drilling. */
static void
on_locals_var_children (G_GNUC_UNUSED MooGdbSession *s,
                        G_GNUC_UNUSED const char *parent,
                        GPtrArray *children,
                        gboolean is_error, gpointer user_data)
{
    LocalsRowCtx *ctx = (LocalsRowCtx *) user_data;
    MooGdbWin *win = ctx->win;
    if (!gtk_tree_row_reference_valid (ctx->row)) {
        locals_row_ctx_free (ctx);
        return;
    }

    GtkTreePath *p = gtk_tree_row_reference_get_path (ctx->row);
    GtkTreeIter parent_it;
    if (!p || !gtk_tree_model_get_iter (
                GTK_TREE_MODEL (win->locals_store), &parent_it, p))
    {
        if (p) gtk_tree_path_free (p);
        locals_row_ctx_free (ctx);
        return;
    }
    gtk_tree_path_free (p);

    /* Wipe any existing children (the dummy placeholder, plus any
     * stale state from an earlier collapse-then-expand). */
    GtkTreeIter child_it;
    while (gtk_tree_model_iter_children (
            GTK_TREE_MODEL (win->locals_store),
            &child_it, &parent_it))
        gtk_tree_store_remove (win->locals_store, &child_it);

    if (is_error || !children) {
        gtk_tree_store_set (win->locals_store, &parent_it,
            LOCALS_COL_LOADED, TRUE, -1);
        locals_row_ctx_free (ctx);
        return;
    }

    for (guint i = 0; i < children->len; i++) {
        MooGdbVarChild *c = (MooGdbVarChild *) children->pdata[i];
        if (!c) continue;
        GtkTreeIter row;
        gtk_tree_store_append (win->locals_store, &row, &parent_it);
        gtk_tree_store_set (win->locals_store, &row,
            LOCALS_COL_NAME,       c->exp   ? c->exp   : (c->name ? c->name : ""),
            LOCALS_COL_TYPE,       c->type  ? c->type  : "",
            LOCALS_COL_VALUE,      c->value ? c->value : (c->numchild > 0 ? "(complex)" : ""),
            LOCALS_COL_EXPR,       c->name  ? c->name  : "",
            LOCALS_COL_VAROBJ,     c->name  ? c->name  : "",
            LOCALS_COL_LOADED,     c->numchild == 0,
            LOCALS_COL_EXPANDABLE, c->numchild > 0,
            -1);
        if (c->numchild > 0)
            locals_add_dummy_child (win->locals_store, &row);
    }

    gtk_tree_store_set (win->locals_store, &parent_it,
        LOCALS_COL_LOADED, TRUE, -1);
    locals_row_ctx_free (ctx);
}

/* Build the locals tree-view widget tree.  Returns the outer
 * scrolled-window for the caller (build_inspect_pane) to mount in
 * the combined right-side pane.  Each row is (name, type, value)
 * plus four bookkeeping columns; rows representing aggregates start
 * collapsed with a placeholder child so the expansion arrow appears. */
static GtkWidget *
build_locals_section (MooGdbWin *win)
{
    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);

    GtkTreeStore *store = gtk_tree_store_new (LOCALS_N_COLS,
                                              G_TYPE_STRING,   /* name */
                                              G_TYPE_STRING,   /* type */
                                              G_TYPE_STRING,   /* value */
                                              G_TYPE_STRING,   /* expr */
                                              G_TYPE_STRING,   /* varobj */
                                              G_TYPE_BOOLEAN,  /* loaded */
                                              G_TYPE_BOOLEAN); /* expandable */

    GtkWidget *view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (view), TRUE);
    g_object_unref (store);

    struct { int idx; const char *label; gboolean expand; } cols[] = {
        { LOCALS_COL_NAME,  "Name",  FALSE },
        { LOCALS_COL_TYPE,  "Type",  FALSE },
        { LOCALS_COL_VALUE, "Value", TRUE  },
    };
    for (int i = 0; i < (int) G_N_ELEMENTS (cols); i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new ();
        if (cols[i].expand)
            g_object_set (r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes (
            cols[i].label, r, "text", cols[i].idx, NULL);
        gtk_tree_view_column_set_resizable (c, TRUE);
        gtk_tree_view_column_set_expand    (c, cols[i].expand);
        gtk_tree_view_append_column (GTK_TREE_VIEW (view), c);
    }

    g_signal_connect (view, "row-expanded",
                      G_CALLBACK (on_locals_row_expanded), win);

    gtk_container_add (GTK_CONTAINER (scroll), view);

    win->locals_pane  = scroll;
    win->locals_store = store;
    win->locals_view  = GTK_TREE_VIEW (view);
    return scroll;
}

/* Session "locals-changed" handler.  Read the current snapshot and
 * push each entry into the tree-store.  Aggregates (rows where gdb's
 * `--simple-values` didn't fill in a value) get a placeholder child
 * so they show an expansion arrow; first expand triggers the var-
 * create round-trip in on_locals_row_expanded. */
static void
on_locals_changed (MooGdbSession *s, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!win->locals_store) return;
    gtk_tree_store_clear (win->locals_store);

    GPtrArray *locals = moo_gdb_session_get_locals (s);
    if (!locals) return;
    for (guint i = 0; i < locals->len; i++) {
        MooGdbLocal *l = (MooGdbLocal *) locals->pdata[i];
        gboolean is_aggregate = (l->value == NULL);
        GtkTreeIter it;
        gtk_tree_store_append (win->locals_store, &it, NULL);
        gtk_tree_store_set (win->locals_store, &it,
            LOCALS_COL_NAME,       l->name  ? l->name  : "",
            LOCALS_COL_TYPE,       l->type  ? l->type  : "",
            LOCALS_COL_VALUE,      l->value ? l->value : "(complex)",
            LOCALS_COL_EXPR,       l->name  ? l->name  : "",
            LOCALS_COL_VAROBJ,     "",
            LOCALS_COL_LOADED,     !is_aggregate,
            LOCALS_COL_EXPANDABLE, is_aggregate,
            -1);
        if (is_aggregate)
            locals_add_dummy_child (win->locals_store, &it);
    }
}

/* ── Stack frames section ────────────────────────────────────────── */

enum {
    FRAMES_COL_LEVEL,    /* int  — frame index, 0 = innermost */
    FRAMES_COL_FUNCTION, /* string */
    FRAMES_COL_LOCATION, /* string — "file:line" or "0xaddr" */
    FRAMES_N_COLS
};

static void
on_frames_row_activated (GtkTreeView *tv, GtkTreePath *path,
                         G_GNUC_UNUSED GtkTreeViewColumn *col,
                         gpointer user_data)
{
    MooGdbWin    *win   = (MooGdbWin *) user_data;
    GtkTreeModel *model = gtk_tree_view_get_model (tv);
    GtkTreeIter   it;
    if (!gtk_tree_model_get_iter (model, &it, path)) return;
    int level = 0;
    gtk_tree_model_get (model, &it, FRAMES_COL_LEVEL, &level, -1);
    if (win->session)
        moo_gdb_session_select_frame (win->session, level);
}

static GtkWidget *
build_frames_section (MooGdbWin *win)
{
    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);

    GtkListStore *store = gtk_list_store_new (FRAMES_N_COLS,
                                              G_TYPE_INT,
                                              G_TYPE_STRING,
                                              G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (view), TRUE);
    g_object_unref (store);

    struct { int idx; const char *label; gboolean expand; } cols[] = {
        { FRAMES_COL_LEVEL,    "#",        FALSE },
        { FRAMES_COL_FUNCTION, "Function", FALSE },
        { FRAMES_COL_LOCATION, "Location", TRUE  },
    };
    for (int i = 0; i < (int) G_N_ELEMENTS (cols); i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new ();
        if (cols[i].expand)
            g_object_set (r, "ellipsize", PANGO_ELLIPSIZE_START, NULL);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes (
            cols[i].label, r, "text", cols[i].idx, NULL);
        gtk_tree_view_column_set_resizable (c, TRUE);
        gtk_tree_view_column_set_expand    (c, cols[i].expand);
        gtk_tree_view_append_column (GTK_TREE_VIEW (view), c);
    }

    /* Double-click a frame to make it the selected frame.  The
     * arrow + Locals pane will refresh to that context. */
    g_signal_connect (view, "row-activated",
                      G_CALLBACK (on_frames_row_activated), win);

    gtk_container_add (GTK_CONTAINER (scroll), view);

    win->frames_pane  = scroll;
    win->frames_store = store;
    return scroll;
}

static void
on_frames_changed (MooGdbSession *s, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!win->frames_store) return;
    gtk_list_store_clear (win->frames_store);

    GPtrArray *frames = moo_gdb_session_get_frames (s);
    if (!frames) return;
    for (guint i = 0; i < frames->len; i++) {
        MooGdbFrame *f = (MooGdbFrame *) frames->pdata[i];
        char *loc;
        if (f->file && f->line > 0) {
            const char *base = strrchr (f->file, '/');
            loc = g_strdup_printf ("%s:%d", base ? base + 1 : f->file,
                                   f->line);
        } else if (f->addr) {
            loc = g_strdup (f->addr);
        } else {
            loc = g_strdup ("");
        }
        GtkTreeIter it;
        gtk_list_store_append (win->frames_store, &it);
        gtk_list_store_set (win->frames_store, &it,
            FRAMES_COL_LEVEL,    f->level,
            FRAMES_COL_FUNCTION, f->function ? f->function : "??",
            FRAMES_COL_LOCATION, loc,
            -1);
        g_free (loc);
    }
}

/* ── Watches section ─────────────────────────────────────────────── */

enum {
    WATCHES_COL_SLOT,        /* int, slot number in session->watches */
    WATCHES_COL_EXPRESSION,
    WATCHES_COL_VALUE,
    WATCHES_COL_ERROR,       /* gboolean — render value red if TRUE */
    WATCHES_N_COLS
};

static void
on_watches_entry_activate (GtkEntry *entry, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    const char *text = gtk_entry_get_text (entry);
    if (!text || !*text) return;
    if (!win->session) {
        console_append (win,
            "Cannot add watch: gdb session not started.  "
            "Press Ctrl+F5 to start debugging first.\n", "error");
        return;
    }
    moo_gdb_session_add_watch (win->session, text);
    gtk_entry_set_text (entry, "");
}

/* Click-to-edit on the Expression column.  GtkCellRendererText
 * fires "edited" with the new string after the user commits via
 * Enter (or Esc to cancel — that doesn't fire "edited" at all).
 * Update the watch slot, then re-issue the eval so the value
 * column refreshes too. */
static void
on_watch_expr_edited (G_GNUC_UNUSED GtkCellRendererText *cell,
                      const gchar *path_str, const gchar *new_text,
                      gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!new_text || !*new_text) return;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
    if (!path) return;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter (GTK_TREE_MODEL (win->watches_store),
                                  &it, path))
    {
        int slot;
        gtk_tree_model_get (GTK_TREE_MODEL (win->watches_store), &it,
            WATCHES_COL_SLOT, &slot, -1);

        /* Update the visible expression cell straight away so the user
         * sees their edit confirmed even if no eval can run yet. */
        gtk_list_store_set (win->watches_store, &it,
            WATCHES_COL_EXPRESSION, new_text,
            WATCHES_COL_VALUE,      "(not yet evaluated)",
            WATCHES_COL_ERROR,      FALSE,
            -1);

        if (win->session && slot >= 0) {
            /* Update the stored expression in the session — we go
             * through the session's GPtrArray directly because there
             * is no public setter, and the slot must keep its identity
             * (its index is used elsewhere as a stable handle). */
            GPtrArray *watches = moo_gdb_session_get_watches (win->session);
            if (watches && (guint) slot < watches->len) {
                MooGdbWatch *w = (MooGdbWatch *) watches->pdata[slot];
                if (w) {
                    g_free (w->expression);
                    w->expression = g_strdup (new_text);
                    g_free (w->value);
                    w->value = NULL;
                    w->error = FALSE;
                }
            }
            /* Trigger an immediate re-eval so the value column
             * refreshes without waiting for the next *stopped. */
            moo_gdb_session_eval_watch (win->session,
                                         (guint) slot, new_text);
        }
    }
    gtk_tree_path_free (path);
}

/* Delete-key handler on the tree-view — remove the selected
 * watch slot.  Returns TRUE if handled (consumed key), FALSE
 * otherwise so other keys keep their default behaviour. */
static gboolean
on_watches_key_press (GtkWidget *widget, GdkEventKey *ev,
                      gpointer user_data)
{
    if (ev->keyval != GDK_KEY_Delete && ev->keyval != GDK_KEY_KP_Delete)
        return FALSE;
    MooGdbWin *win = (MooGdbWin *) user_data;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
    GtkTreeModel *model;
    GtkTreeIter   it;
    if (!gtk_tree_selection_get_selected (sel, &model, &it))
        return FALSE;
    int slot;
    gtk_tree_model_get (model, &it, WATCHES_COL_SLOT, &slot, -1);
    if (win->session && slot >= 0)
        moo_gdb_session_remove_watch (win->session, (guint) slot);
    return TRUE;
}

static GtkWidget *
build_watches_section (MooGdbWin *win)
{
    GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand (scroll, TRUE);
    gtk_widget_set_vexpand (scroll, TRUE);

    GtkListStore *store = gtk_list_store_new (WATCHES_N_COLS,
                                              G_TYPE_INT,
                                              G_TYPE_STRING,
                                              G_TYPE_STRING,
                                              G_TYPE_BOOLEAN);
    GtkWidget *view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (view), TRUE);
    g_object_unref (store);

    /* Expression column — editable.  Clicking once selects the row,
     * double-click or F2 opens the in-place editor.  GtkCellRendererText
     * fires "edited" with the new value on Enter; Esc cancels silently. */
    {
        GtkCellRenderer *r = gtk_cell_renderer_text_new ();
        g_object_set (r, "editable", TRUE, NULL);
        g_signal_connect (r, "edited",
                          G_CALLBACK (on_watch_expr_edited), win);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes (
            "Expression", r, "text", WATCHES_COL_EXPRESSION, NULL);
        gtk_tree_view_column_set_resizable (c, TRUE);
        gtk_tree_view_append_column (GTK_TREE_VIEW (view), c);
    }
    {
        GtkCellRenderer *r = gtk_cell_renderer_text_new ();
        g_object_set (r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes (
            "Value", r,
            "text",             WATCHES_COL_VALUE,
            NULL);
        /* Bind the error column to the renderer's foreground via a
         * cell-data-func so we colour failures red.  Simpler: use
         * tree_view_column_add_attribute with foreground-set. */
        gtk_tree_view_column_set_cell_data_func (c, r,
            [](GtkTreeViewColumn *, GtkCellRenderer *cell,
               GtkTreeModel *m, GtkTreeIter *iter, gpointer) {
                gboolean err = FALSE;
                gtk_tree_model_get (m, iter, WATCHES_COL_ERROR, &err, -1);
                g_object_set (cell,
                    "foreground", err ? "#c0392b" : NULL,
                    "foreground-set", err,
                    NULL);
            }, NULL, NULL);
        gtk_tree_view_column_set_resizable (c, TRUE);
        gtk_tree_view_column_set_expand    (c, TRUE);
        gtk_tree_view_append_column (GTK_TREE_VIEW (view), c);
    }

    g_signal_connect (view, "key-press-event",
                      G_CALLBACK (on_watches_key_press), win);

    gtk_container_add (GTK_CONTAINER (scroll), view);
    gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 0);

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry),
        _("Type a C expression and press Enter (Delete to remove a row)"));
    gtk_box_pack_start (GTK_BOX (vbox), entry, FALSE, FALSE, 0);
    g_signal_connect (entry, "activate",
                      G_CALLBACK (on_watches_entry_activate), win);

    win->watches_pane  = vbox;
    win->watches_store = store;
    win->watches_view  = GTK_TREE_VIEW (view);
    win->watches_entry = GTK_ENTRY (entry);
    return vbox;
}

/* Build the single combined right-side pane that stacks Locals,
 * Stack, and Watches as three resizable sections.  Each section has
 * a bold header label so the user can tell them apart without
 * relying on a tab control.  Two nested GtkPaneds give independent
 * drag-to-resize between adjacent sections — the top split scales
 * Locals against everything else, the bottom split scales Stack
 * against Watches.  Both default to ~33%/33%/33% via the
 * `position-set=FALSE` initial state which lets GTK compute even
 * splits based on natural-size hints. */
/* Each inspect-pane section gets its own GtkExpander so the user
 * can fold what they're not paying attention to.  Sections start
 * expanded so the default look matches the previous (always-shown)
 * layout.  The body is still vexpand=TRUE so when an expander is
 * open it occupies its share of the surrounding GtkPaned; when
 * folded it shrinks to the header height and the user can drag
 * the paned divider to give the still-open sections more room.
 *
 * Note: hiding a section does NOT throttle gdb traffic — the
 * session refresh on *stopped is a few cheap MI commands that
 * happen whether or not the rows are visible.  Folding is a
 * visual-focus aid, not a throughput optimisation. */
static GtkWidget *
wrap_section (const char *title, GtkWidget *body)
{
    GtkWidget *expander = gtk_expander_new (NULL);
    char *markup = g_markup_printf_escaped ("<b>%s</b>", title);
    gtk_expander_set_use_markup (GTK_EXPANDER (expander), TRUE);
    gtk_expander_set_label      (GTK_EXPANDER (expander), markup);
    g_free (markup);
    gtk_expander_set_expanded   (GTK_EXPANDER (expander), TRUE);
    gtk_widget_set_hexpand (body, TRUE);
    gtk_widget_set_vexpand (body, TRUE);
    gtk_container_add (GTK_CONTAINER (expander), body);
    return expander;
}

/* One row of the toolbar above the inspect pane.  `icon_name` is a
 * freedesktop icon-spec name; we let the active theme resolve it.
 * `NULL` for icon_name marks a separator.  The action signature for
 * `cb` matches the existing per-window forwards (moo_gdb_win_*) so
 * we can connect with g_signal_connect_swapped and skip the
 * boilerplate of writing per-button trampolines. */
typedef struct {
    const char *icon_name;
    const char *label;
    const char *tooltip;
    void (*cb) (MooGdbWin *);
} ToolBtnSpec;

/* Walk a widget tree and apply a tooltip to every node.  GtkToolButton
 * wraps an inner GtkButton, which in turn wraps a GtkBox containing
 * a GtkImage and a GtkLabel.  Each of these descendants has its own
 * input window that can swallow pointer motion before it bubbles to
 * an ancestor with `has-tooltip` set — so a tooltip applied only to
 * the outer GtkToolItem never fires when the cursor sits on the icon
 * or label.  Painting the tooltip onto every descendant guarantees a
 * match regardless of which sub-widget receives the motion event. */
static void
set_tooltip_recursive (GtkWidget *widget, const char *text)
{
    if (!widget) return;
    gtk_widget_set_tooltip_text (widget, text);
    if (GTK_IS_CONTAINER (widget)) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
        for (GList *l = children; l; l = l->next)
            set_tooltip_recursive (GTK_WIDGET (l->data), text);
        g_list_free (children);
    }
}

/* Tooltip strings are deliberately multi-line: first line is the
 * action name (matches the menu label), second describes the
 * effect, third shows the keyboard shortcut. */
static const ToolBtnSpec INSPECT_TOOLBAR[] = {
    { "media-playback-start", "Start",
      "Start Debugging\n"
      "Launch the active configuration's program under gdb; "
      "auto-builds first if the binary is stale.\n"
      "Shortcut: Ctrl+F5",
      moo_gdb_win_start },
    { "system-run",           "Build",
      "Build\n"
      "Run the active configuration's preLaunchTask (or its "
      "`build` shorthand) — forces a rebuild regardless of "
      "binary staleness.\n"
      "Output streams to the GDB Console pane.",
      moo_gdb_win_build },
    { "go-next",              "Continue",
      "Continue\n"
      "Resume execution until the next breakpoint, signal, or exit.\n"
      "Shortcut: Ctrl+F8",
      moo_gdb_win_continue },
    { "media-playback-pause", "Pause",
      "Pause Execution\n"
      "Send SIGINT to the running inferior so it stops at its "
      "current source line.\n"
      "Shortcut: F6",
      moo_gdb_win_pause },
    { "media-playback-stop",  "Stop",
      "Stop Debugging\n"
      "Kill the inferior and quit gdb.  The next Start Debugging "
      "will spawn a fresh session.",
      moo_gdb_win_stop },
    { NULL, NULL, NULL, NULL },   /* separator */
    { "go-down",              "Step Into",
      "Step Into\n"
      "Execute one source line; descend into function calls.\n"
      "Shortcut: F11",
      moo_gdb_win_step_into },
    { "go-jump",              "Step Over",
      "Step Over\n"
      "Execute one source line; treat function calls as atomic.\n"
      "Shortcut: F10",
      moo_gdb_win_step_over },
    { "go-up",                "Step Out",
      "Step Out\n"
      "Run until the current function returns, then stop.\n"
      "Shortcut: Shift+F11",
      moo_gdb_win_step_out },
    { NULL, NULL, NULL, NULL },   /* separator */
    { "process-stop",         "Toggle Breakpoint",
      "Toggle Breakpoint\n"
      "Add or remove a breakpoint at the cursor's line in the "
      "active document.  A red dot marks each set breakpoint in "
      "the gutter.\n"
      "Shortcut: F9",
      moo_gdb_win_toggle_bp_at_cursor },
    { "preferences-system",   "Configure",
      "Configure\n"
      "When a project is loaded, opens its launch.json in medit "
      "so you can edit configurations directly.  Otherwise pops "
      "the legacy Target / Args / Cwd dialog.",
      moo_gdb_win_configure },
};

/* ── Configurations dropdown / project lifecycle ─────────────────── */

static void
on_cfg_combo_changed (GtkComboBox *combo, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    win->active_config = gtk_combo_box_get_active (combo);
}

/* Refresh the dropdown's contents to match the currently-loaded
 * project.  Pre-emptively blocks the "changed" handler so re-
 * populating doesn't fire spurious notifications. */
static void
refresh_cfg_combo (MooGdbWin *win)
{
    if (!win->cfg_combo) return;
    g_signal_handlers_block_by_func (
        win->cfg_combo, (gpointer) on_cfg_combo_changed, win);

    gtk_combo_box_text_remove_all (win->cfg_combo);

    guint n = win->project ? moo_gdb_project_n_configs (win->project) : 0;
    for (guint i = 0; i < n; i++) {
        const MooGdbConfig *c = moo_gdb_project_config (win->project, i);
        gtk_combo_box_text_append_text (win->cfg_combo,
            c->name ? c->name : "(unnamed)");
    }
    gtk_widget_set_sensitive (GTK_WIDGET (win->cfg_combo), n > 0);
    if (n > 0) {
        int idx = win->active_config;
        if (idx < 0 || (guint) idx >= n) idx = 0;
        gtk_combo_box_set_active (GTK_COMBO_BOX (win->cfg_combo), idx);
        win->active_config = idx;
    } else {
        win->active_config = -1;
    }

    if (win->cfg_label) {
        if (n > 0 && win->project) {
            char *root = g_path_get_basename (
                moo_gdb_project_root (win->project));
            char *txt = g_strdup_printf ("Config (%s):", root);
            gtk_label_set_text (win->cfg_label, txt);
            g_free (txt);
            g_free (root);
        } else {
            gtk_label_set_text (win->cfg_label, "Config: (none)");
        }
    }

    g_signal_handlers_unblock_by_func (
        win->cfg_combo, (gpointer) on_cfg_combo_changed, win);
}

/* Walk up from `start_path` and (re)load the project there.
 * `clear_if_missing`: when TRUE and no project is found, also drop
 * any previously-loaded project (used by the active-doc handler so
 * the dropdown empties when the user switches to a file outside the
 * project tree).  When FALSE, a missing project leaves the existing
 * one alone (used by the new-doc handler so opening a non-project
 * file from File→Open doesn't blow away the user's config). */
static void
reload_project_from_path (MooGdbWin  *win,
                          const char *start_path,
                          gboolean    clear_if_missing)
{
    if (!start_path) {
        if (clear_if_missing && win->project) {
            moo_gdb_project_free (win->project);
            win->project = NULL;
            refresh_cfg_combo (win);
        }
        return;
    }
    GError *err = NULL;
    MooGdbProject *p = moo_gdb_project_load (start_path, &err);

    if (!p) {
        /* No project found — that's normal, don't spam the console.
         * Only complain if a launch.json existed but failed to parse. */
        if (err && err->code != 0)
            console_append (win, err->message, "error");
        if (err) g_error_free (err);
        if (clear_if_missing && win->project) {
            moo_gdb_project_free (win->project);
            win->project = NULL;
            refresh_cfg_combo (win);
        }
        return;
    }
    if (win->project) moo_gdb_project_free (win->project);
    win->project = p;
    refresh_cfg_combo (win);
}

/* (Re)discover the project from the active document's path and
 * refresh the dropdown.  No-op if no document is active — the
 * previous project, if any, stays loaded so users can swap to a
 * non-source file without losing their config. */
static void
reload_project (MooGdbWin *win)
{
    MooEdit *doc = moo_edit_window_get_active_doc (win->window);
    if (!doc) return;
    char *file = moo_edit_get_filename (doc);
    reload_project_from_path (win, file, /*clear_if_missing=*/TRUE);
    g_free (file);
}

static void
on_active_doc_notify (G_GNUC_UNUSED GObject *obj,
                      G_GNUC_UNUSED GParamSpec *pspec,
                      gpointer user_data)
{
    reload_project ((MooGdbWin *) user_data);
}

/* Re-discover the project whenever a doc is loaded into the window.
 * Fires unconditionally — even if a project is already loaded and
 * the new doc lives under the same root — because the user may have
 * just edited launch.json on disk and re-opened the file to pick up
 * the change.  reload_project_from_path frees the previous project
 * first, so the on-disk launch.json is always re-read here.
 *
 * NOTE: this signal fires BEFORE moo_edit_window_set_active_doc
 * (see mooeditwindow.cpp _moo_edit_window_insert_doc), so the new
 * doc is not yet the window's active doc.  We therefore can't call
 * reload_project (which reads get_active_doc and would see the old /
 * NULL value); instead we use the doc handed to us by the signal
 * directly.  This is what was breaking `medit foo.c` from the CLI —
 * the initial reload_project at plugin-create time saw no active doc,
 * and the subsequent new-doc signal looked up the wrong one. */
static void
on_new_doc (G_GNUC_UNUSED MooEditWindow *window,
            MooEdit *doc,
            gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!doc) return;
    char *file = moo_edit_get_filename (doc);
    reload_project_from_path (win, file, /*clear_if_missing=*/FALSE);
    g_free (file);
}

static GtkWidget *
build_inspect_toolbar (MooGdbWin *win)
{
    GtkWidget *toolbar = gtk_toolbar_new ();
    gtk_toolbar_set_style     (GTK_TOOLBAR (toolbar), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size (GTK_TOOLBAR (toolbar),
                                GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_toolbar_set_show_arrow (GTK_TOOLBAR (toolbar), TRUE);

    /* Configurations dropdown — appears at the start of the toolbar
     * so the user picks "what to debug" before the action buttons.
     * Wrapped in a GtkToolItem (the toolbar's required child type). */
    {
        GtkToolItem *lbl_item = gtk_tool_item_new ();
        GtkWidget   *lbl      = gtk_label_new ("Config: (none)");
        g_object_set (lbl, "margin-start", 4, "margin-end", 4, NULL);
        gtk_container_add (GTK_CONTAINER (lbl_item), lbl);
        gtk_tool_item_set_tooltip_text (lbl_item,
            "Project root — directory containing .medit/launch.json "
            "or .vscode/launch.json.  Shows '(none)' when no project "
            "file was found upwards from the active document.");
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), lbl_item, -1);
        win->cfg_label = GTK_LABEL (lbl);

        GtkToolItem *combo_item = gtk_tool_item_new ();
        GtkWidget   *combo      = gtk_combo_box_text_new ();
        gtk_widget_set_sensitive (combo, FALSE);   /* until a project loads */
        gtk_tool_item_set_tooltip_text (combo_item,
            "Pick a launch configuration from launch.json.  The "
            "selected entry's program / args / cwd / environment "
            "are used by Start Debugging and Build.");
        g_signal_connect (combo, "changed",
                          G_CALLBACK (on_cfg_combo_changed), win);
        gtk_container_add (GTK_CONTAINER (combo_item), combo);
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), combo_item, -1);
        win->cfg_combo = GTK_COMBO_BOX_TEXT (combo);

        GtkToolItem *sep = gtk_separator_tool_item_new ();
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), sep, -1);
    }

    for (guint i = 0; i < G_N_ELEMENTS (INSPECT_TOOLBAR); i++) {
        const ToolBtnSpec *spec = &INSPECT_TOOLBAR[i];
        GtkToolItem *item;
        if (!spec->icon_name) {
            item = gtk_separator_tool_item_new ();
        } else {
            item = gtk_tool_button_new (NULL, spec->label);
            gtk_tool_button_set_icon_name (
                GTK_TOOL_BUTTON (item), spec->icon_name);
            /* See set_tooltip_recursive — paint the tooltip onto the
             * tool item and every descendant so any hovered sub-widget
             * resolves the tooltip via its own input window. */
            set_tooltip_recursive (GTK_WIDGET (item), spec->tooltip);
            /* g_signal_connect_swapped flips the argument order so
             * the per-window callback gets called as `cb(win)` —
             * matches the existing moo_gdb_win_* function signatures
             * verbatim, no per-button trampoline needed. */
            g_signal_connect_swapped (item, "clicked",
                                       G_CALLBACK (spec->cb), win);
        }
        gtk_toolbar_insert (GTK_TOOLBAR (toolbar), item, -1);
    }
    return toolbar;
}

static void
build_inspect_pane (MooGdbWin *win)
{
    GtkWidget *locals  = wrap_section (_("Locals"),
                                       build_locals_section (win));
    GtkWidget *frames  = wrap_section (_("Call Stack"),
                                       build_frames_section (win));
    GtkWidget *watches = wrap_section (_("Watches"),
                                       build_watches_section (win));

    /* Outer paned: Locals on top, (Stack+Watches) on bottom.
     * Inner paned: Stack on top, Watches on bottom. */
    GtkWidget *inner = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1 (GTK_PANED (inner), frames,  TRUE, FALSE);
    gtk_paned_pack2 (GTK_PANED (inner), watches, TRUE, FALSE);

    GtkWidget *outer = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1 (GTK_PANED (outer), locals, TRUE, FALSE);
    gtk_paned_pack2 (GTK_PANED (outer), inner,  TRUE, FALSE);

    /* Wrap in a vbox so the debugger toolbar can ride above the
     * splits without resizing when the user drags a pane divider. */
    GtkWidget *vbox    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *toolbar = build_inspect_toolbar (win);
    gtk_box_pack_start (GTK_BOX (vbox), toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), outer,   TRUE,  TRUE,  0);

    gtk_widget_show_all (vbox);

    MooPaneLabel *label = moo_pane_label_new ("system-search", NULL,
                                              _("Debugger"),
                                              _("Locals, Call Stack, Watches"));
    moo_edit_window_add_pane (win->window, MOO_GDB_INSPECT_PANE_ID,
                              vbox, label, MOO_PANE_POS_RIGHT);
    moo_pane_label_free (label);
}

static void
on_watches_changed (MooGdbSession *s, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!win->watches_store) return;
    gtk_list_store_clear (win->watches_store);

    GPtrArray *watches = moo_gdb_session_get_watches (s);
    if (!watches) return;
    for (guint i = 0; i < watches->len; i++) {
        MooGdbWatch *w = (MooGdbWatch *) watches->pdata[i];
        if (!w) continue;   /* removed slot */
        GtkTreeIter it;
        gtk_list_store_append (win->watches_store, &it);
        gtk_list_store_set (win->watches_store, &it,
            WATCHES_COL_SLOT,       (int) i,
            WATCHES_COL_EXPRESSION, w->expression ? w->expression : "",
            WATCHES_COL_VALUE,
                w->value ? w->value : (w->expression ? "(not yet evaluated)" : ""),
            WATCHES_COL_ERROR,      w->error,
            -1);
    }
}

/* ── Hover-to-inspect ────────────────────────────────────────────────
 *
 * On every *stopped event we walk the window's open documents and
 * install a "query-tooltip" handler on each MooEditView.  The handler
 * extracts the C identifier under the mouse, looks it up in
 * `hover_cache`, and either shows the cached value or kicks off a
 * one-shot `-data-evaluate-expression` whose reply caches the value
 * and re-triggers the tooltip query.
 *
 * Identifier-only — no `->`, no `.`, no array indexing for v1.  The
 * extracted text is wrapped in double quotes when passed to gdb;
 * that's safe because we only accept [_A-Za-z][_A-Za-z0-9]*. */

/* GObject-data key.  Used to mark a view as already hooked so the
 * stop handler can re-iterate without double-attaching. */
#define MOO_GDB_HOVER_HOOKED_KEY  "moogdb-hover-hooked"

typedef struct {
    MooGdbWin *win;
    char      *expr;
} HoverCtx;

static gboolean
is_c_ident_char (gunichar c, gboolean first)
{
    if (c == '_') return TRUE;
    if (first) return g_unichar_isalpha (c);
    return g_unichar_isalnum (c);
}

/* Walk left and right from `iter` collecting the maximal run of
 * C-identifier characters that includes the iter's position.
 * Returns a freshly-allocated string (NULL if no identifier here). */
static char *
extract_identifier_at (GtkTextIter *iter)
{
    gunichar ch = gtk_text_iter_get_char (iter);
    if (!is_c_ident_char (ch, FALSE))
        return NULL;
    GtkTextIter start = *iter, end = *iter;
    /* Walk left while previous char is still an ident char. */
    while (gtk_text_iter_backward_char (&start)) {
        gunichar c = gtk_text_iter_get_char (&start);
        if (!is_c_ident_char (c, FALSE)) {
            gtk_text_iter_forward_char (&start);
            break;
        }
    }
    /* Forward end past the last ident char. */
    while (is_c_ident_char (gtk_text_iter_get_char (&end), FALSE)) {
        if (!gtk_text_iter_forward_char (&end))
            break;
    }
    /* Reject identifiers starting with a digit (would be a number,
     * not a variable). */
    gunichar first = gtk_text_iter_get_char (&start);
    if (!is_c_ident_char (first, TRUE))
        return NULL;
    return gtk_text_buffer_get_text (gtk_text_iter_get_buffer (&start),
                                      &start, &end, FALSE);
}

/* Reply callback for the async hover evaluation.  Stash the result
 * in the cache and re-trigger the tooltip so the user sees the new
 * value if they're still hovering. */
static void
on_hover_eval_reply (G_GNUC_UNUSED MooGdbSession *s,
                     const char *expr, const char *value,
                     gboolean is_error, gpointer user_data)
{
    HoverCtx *ctx = (HoverCtx *) user_data;
    MooGdbWin *win = ctx->win;
    if (!win->hover_cache || !expr) {
        g_free (ctx->expr);
        g_free (ctx);
        return;
    }
    /* Prefix errors with "!" so the hover handler can render them
     * differently from a successful value if we want to later.
     * For now both render as plain tooltip text. */
    char *cached;
    if (is_error)
        cached = g_strdup_printf ("!%s", value ? value : "evaluation failed");
    else
        cached = g_strdup (value ? value : "(null)");
    g_hash_table_insert (win->hover_cache, g_strdup (expr), cached);
    g_hash_table_remove (win->hover_pending, expr);

    /* Re-fire the tooltip query for whichever view the user is
     * currently hovering over — if it's still us and still on this
     * identifier, the new value will appear.  We don't know which
     * view spawned the request, so trigger on all hooked ones. */
    MooEditor *editor = moo_editor_instance ();
    MooEditArray *docs = moo_editor_get_docs (editor);
    if (docs) {
        for (guint i = 0; i < docs->n_elms; i++) {
            MooEditView *v = moo_edit_get_view (docs->elms[i]);
            if (v && g_object_get_data (G_OBJECT (v),
                                         MOO_GDB_HOVER_HOOKED_KEY))
                gtk_widget_trigger_tooltip_query (GTK_WIDGET (v));
        }
        moo_edit_array_free (docs);
    }
    g_free (ctx->expr);
    g_free (ctx);
}

/* "query-tooltip" handler installed on each MooEditView while a
 * session is active.  GtkTextView passes window-relative (x,y) on
 * mouse-driven queries and the cursor position on keyboard-driven
 * ones — convert to buffer coords either way before pulling the
 * identifier out. */
static gboolean
on_view_query_tooltip (GtkWidget *widget, gint x, gint y,
                        gboolean keyboard_mode,
                        GtkTooltip *tooltip, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (!win || !win->session) return FALSE;
    if (moo_gdb_session_get_state (win->session) != MOO_GDB_STATE_STOPPED)
        return FALSE;
    /* After the inferior exits, gdb keeps the session alive (so the
     * user can restart) but there's no frame — every evaluation
     * would come back as "No registers."  Skip silently. */
    if (!moo_gdb_session_inferior_alive (win->session))
        return FALSE;

    GtkTextView *view = GTK_TEXT_VIEW (widget);
    GtkTextIter iter;
    if (keyboard_mode) {
        gtk_text_buffer_get_iter_at_mark (
            gtk_text_view_get_buffer (view), &iter,
            gtk_text_buffer_get_insert (gtk_text_view_get_buffer (view)));
    } else {
        gint bx, by;
        gtk_text_view_window_to_buffer_coords (
            view, GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
        if (!gtk_text_view_get_iter_at_location (view, &iter, bx, by))
            return FALSE;
    }

    char *expr = extract_identifier_at (&iter);
    if (!expr) return FALSE;
    /* Filter obvious non-variables (C keywords / common type names)
     * so we don't waste a gdb round-trip on `int`, `if`, etc. */
    static const char *const KEYWORDS[] = {
        "if","else","for","while","do","switch","case","break",
        "continue","return","goto","sizeof","typedef","struct","union",
        "enum","static","extern","auto","register","const","volatile",
        "void","char","short","int","long","float","double","signed",
        "unsigned","NULL","TRUE","FALSE","true","false", NULL
    };
    for (const char *const *k = KEYWORDS; *k; k++)
        if (!strcmp (expr, *k)) { g_free (expr); return FALSE; }

    const char *cached = (const char *)
        g_hash_table_lookup (win->hover_cache, expr);
    if (cached) {
        /* "!msg" → error message; render with the same tooltip but
         * could be styled differently in future. */
        const char *display = (cached[0] == '!') ? cached + 1 : cached;
        char *line = g_strdup_printf ("%s = %s", expr, display);
        gtk_tooltip_set_text (tooltip, line);
        g_free (line);
        g_free (expr);
        return TRUE;
    }

    /* No cached value yet.  Kick off an async eval if not already
     * pending; show "..." in the meantime.  The reply will
     * re-trigger the tooltip query so the value lands without the
     * user having to mouse off and back. */
    if (!g_hash_table_contains (win->hover_pending, expr)) {
        g_hash_table_insert (win->hover_pending, g_strdup (expr),
                              GINT_TO_POINTER (1));
        HoverCtx *ctx = g_new0 (HoverCtx, 1);
        ctx->win  = win;
        ctx->expr = g_strdup (expr);
        moo_gdb_session_eval_async (win->session, expr,
                                     on_hover_eval_reply, ctx);
    }
    char *line = g_strdup_printf ("%s = ...", expr);
    gtk_tooltip_set_text (tooltip, line);
    g_free (line);
    g_free (expr);
    return TRUE;
}

/* Attach the query-tooltip handler to `view` if not already done.
 * Idempotent — we mark the view with set_data after the first call.
 * Called on every *stopped because new docs may have been opened
 * since the last stop. */
/* A hooked view can be closed while the debug window lives; forget it so
 * detach_hover_from_views() does not touch a destroyed widget. */
static void
on_hover_view_destroy (GtkWidget *view, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    if (win->hover_views)
        g_ptr_array_remove_fast (win->hover_views, view);
}

static void
attach_hover_to_view (MooGdbWin *win, MooEditView *view)
{
    if (!view) return;
    if (g_object_get_data (G_OBJECT (view), MOO_GDB_HOVER_HOOKED_KEY))
        return;
    gtk_widget_set_has_tooltip (GTK_WIDGET (view), TRUE);
    g_signal_connect (view, "query-tooltip",
                      G_CALLBACK (on_view_query_tooltip), win);
    g_signal_connect (view, "destroy",
                      G_CALLBACK (on_hover_view_destroy), win);
    g_object_set_data (G_OBJECT (view), MOO_GDB_HOVER_HOOKED_KEY,
                       GINT_TO_POINTER (1));
    g_ptr_array_add (win->hover_views, view);
}

/* Undo attach_hover_to_view for every still-live view.  Without this the
 * handlers stay connected to documents that outlive the debug window, and
 * hovering any of them dereferences the freed MooGdbWin; the stale
 * HOOKED_KEY would also stop a new window from ever re-attaching. */
static void
detach_hover_from_views (MooGdbWin *win)
{
    if (!win->hover_views) return;
    for (guint i = 0; i < win->hover_views->len; i++) {
        GObject *view = G_OBJECT (win->hover_views->pdata[i]);
        g_signal_handlers_disconnect_by_data (view, win);
        g_object_set_data (view, MOO_GDB_HOVER_HOOKED_KEY, NULL);
    }
    g_ptr_array_free (win->hover_views, TRUE);
    win->hover_views = NULL;
}

static void
attach_hover_to_open_docs (MooGdbWin *win)
{
    MooEditor *editor = moo_editor_instance ();
    MooEditArray *docs = moo_editor_get_docs (editor);
    if (!docs) return;
    for (guint i = 0; i < docs->n_elms; i++)
        attach_hover_to_view (win, moo_edit_get_view (docs->elms[i]));
    moo_edit_array_free (docs);
}

/* ── Console pane ─────────────────────────────────────────────────── */

#define MOO_GDB_CONSOLE_PANE_ID "MooGdbConsole"

/* Append `text` to the console buffer with optional `tag_name`
 * for colour coding.  Auto-scrolls so the latest line is visible. */
static void
console_append (MooGdbWin *win, const char *text, const char *tag_name)
{
    if (!win->console_buffer || !text || !*text) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter (win->console_buffer, &end);
    if (tag_name)
        gtk_text_buffer_insert_with_tags_by_name (
            win->console_buffer, &end, text, -1, tag_name, NULL);
    else
        gtk_text_buffer_insert (win->console_buffer, &end, text, -1);

    /* Scroll the view so the bottom stays in sight even when the
     * user hasn't focused the pane recently. */
    if (win->console_view) {
        GtkTextMark *insert =
            gtk_text_buffer_get_insert (win->console_buffer);
        gtk_text_view_scroll_mark_onscreen (win->console_view, insert);
    }
}

static void
on_console_signal (G_GNUC_UNUSED MooGdbSession *s,
                   const char *line, gpointer user_data)
{
    console_append ((MooGdbWin *) user_data, line, NULL);
}

static void
on_log_signal (G_GNUC_UNUSED MooGdbSession *s,
               const char *line, gpointer user_data)
{
    console_append ((MooGdbWin *) user_data, line, "log");
}

static void
on_target_signal (G_GNUC_UNUSED MooGdbSession *s,
                  const char *line, gpointer user_data)
{
    /* Inferior's own stdout/stderr — what the user expects to see
     * when their program prints something.  Style it with the
     * "target" tag so it visually separates from gdb's chatter. */
    console_append ((MooGdbWin *) user_data, line, "target");
}

static void
on_console_error (G_GNUC_UNUSED MooGdbSession *s,
                  const char *msg, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    char *line = g_strdup_printf ("error: %s\n", msg ? msg : "(unknown)");
    console_append (win, line, "error");
    g_free (line);
}

/* User pressed Enter in the entry — forward the line to gdb, then
 * echo it locally so the transcript reads coherently. */
static void
on_console_entry_activate (GtkEntry *entry, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    const char *text = gtk_entry_get_text (entry);
    if (!text || !*text) return;
    /* Echo with a leading "(gdb) " so the user can tell their
     * commands apart from gdb's responses. */
    char *echo = g_strdup_printf ("(gdb) %s\n", text);
    console_append (win, echo, "input");
    g_free (echo);
    if (win->session)
        moo_gdb_session_send_raw (win->session, text);
    gtk_entry_set_text (entry, "");
}

/* Build the console pane widget tree.  Layout:
 *
 *   ┌──────────────────────────────────────┐
 *   │ scrolled-window                       │
 *   │  └ GtkTextView (read-only, mono)      │
 *   ├──────────────────────────────────────┤
 *   │ GtkEntry  ← user types here           │
 *   └──────────────────────────────────────┘
 *
 * Wrapped in a vbox so the entry stays a single line at the bottom
 * regardless of content size in the text view. */
static void
build_console_pane (MooGdbWin *win)
{
    GtkWidget *vbox   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand (scroll, TRUE);
    gtk_widget_set_vexpand (scroll, TRUE);

    GtkWidget *view = gtk_text_view_new ();
    gtk_text_view_set_editable      (GTK_TEXT_VIEW (view), FALSE);
    gtk_text_view_set_monospace     (GTK_TEXT_VIEW (view), TRUE);
    gtk_text_view_set_left_margin   (GTK_TEXT_VIEW (view), 6);
    gtk_text_view_set_right_margin  (GTK_TEXT_VIEW (view), 6);
    gtk_text_view_set_top_margin    (GTK_TEXT_VIEW (view), 4);
    gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (view), 4);
    gtk_text_view_set_wrap_mode     (GTK_TEXT_VIEW (view),
                                     GTK_WRAP_WORD_CHAR);
    gtk_container_add (GTK_CONTAINER (scroll), view);
    gtk_box_pack_start (GTK_BOX (vbox), scroll, TRUE, TRUE, 0);

    /* Three tags for the three stream kinds: dimmed for log lines,
     * red-ish for errors, italic-grey for echoed user input. */
    GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    gtk_text_buffer_create_tag (buf, "log",
        "foreground", "#7f7f7f", NULL);
    gtk_text_buffer_create_tag (buf, "error",
        "foreground", "#c0392b",
        "weight",     PANGO_WEIGHT_BOLD,
        NULL);
    gtk_text_buffer_create_tag (buf, "input",
        "foreground", "#1a73e8",
        "style",      PANGO_STYLE_ITALIC,
        NULL);
    /* Inferior's own stdout/stderr — keep readable but visually
     * distinct from gdb's chatter so the user can tell their
     * program's printf output apart at a glance. */
    gtk_text_buffer_create_tag (buf, "target",
        "foreground", "#27ae60",
        NULL);
    /* Build-task output (preLaunchTask stdout/stderr) — orange so
     * it doesn't clash with gdb's `target` green or `error` red. */
    gtk_text_buffer_create_tag (buf, "build",
        "foreground", "#d35400",
        NULL);

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry),
        "Type a gdb command (e.g. `print x`, `info threads`) and press Enter");
    gtk_box_pack_start (GTK_BOX (vbox), entry, FALSE, FALSE, 0);
    g_signal_connect (entry, "activate",
        G_CALLBACK (on_console_entry_activate), win);

    gtk_widget_show_all (vbox);

    MooPaneLabel *label = moo_pane_label_new ("utilities-terminal", NULL,
                                              _("GDB Console"),
                                              _("GDB Console"));
    moo_edit_window_add_pane (win->window, MOO_GDB_CONSOLE_PANE_ID,
                              vbox, label, MOO_PANE_POS_BOTTOM);
    moo_pane_label_free (label);

    win->console_pane   = vbox;
    win->console_buffer = buf;
    win->console_view   = GTK_TEXT_VIEW (view);
    win->console_entry  = GTK_ENTRY (entry);
}

static void detach_visual_mark (GdbBreakpoint *bp);

static void
free_bp (gpointer p)
{
    GdbBreakpoint *bp = (GdbBreakpoint *) p;
    if (!bp) return;
    g_free (bp->file);
    /* The buffer holds its own reference to the mark, so dropping ours is not
       enough: without delete_line_mark the red dot stays in the gutter forever
       once the debug window goes away, with no way to clear it. */
    detach_visual_mark (bp);
    g_free (bp);
}

static void
free_file_table (gpointer p)
{
    g_hash_table_destroy ((GHashTable *) p);
}

/* Look up an existing breakpoint at file:line, or NULL. */
static GdbBreakpoint *
lookup_bp (MooGdbWin *win, const char *file, int line)
{
    if (!win->bp_by_file || !file) return NULL;
    GHashTable *line_tbl = (GHashTable *)
        g_hash_table_lookup (win->bp_by_file, file);
    if (!line_tbl) return NULL;
    return (GdbBreakpoint *)
        g_hash_table_lookup (line_tbl, GINT_TO_POINTER (line));
}

static void
register_bp (MooGdbWin *win, GdbBreakpoint *bp)
{
    /* bp->file must be non-NULL — g_str_hash requires a real C
     * string.  Caller is expected to filter NULL paths before
     * calling us; this is a belt-and-braces guard. */
    if (!bp->file) {
        free_bp (bp);
        return;
    }
    GHashTable *line_tbl = (GHashTable *)
        g_hash_table_lookup (win->bp_by_file, bp->file);
    if (!line_tbl) {
        line_tbl = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                          NULL, free_bp);
        g_hash_table_insert (win->bp_by_file, g_strdup (bp->file), line_tbl);
    }
    g_hash_table_insert (line_tbl, GINT_TO_POINTER (bp->line), bp);
    if (bp->number >= 0)
        g_hash_table_insert (win->bp_by_number,
                              GINT_TO_POINTER (bp->number), bp);
}

/* Find the open MooEditView showing `file` (if any) and place a red
 * dot in its gutter at `line`.  Safe to call before/after the
 * actual file is loaded — we'll re-place on doc-loaded events too. */
static void
attach_visual_mark (G_GNUC_UNUSED MooGdbWin *win, GdbBreakpoint *bp)
{
    if (bp->mark) return;
    MooEditor *editor = moo_editor_instance ();
    /* moo_editor_get_docs returns transfer-full — caller frees with
     * moo_edit_array_free. */
    MooEditArray *docs = moo_editor_get_docs (editor);
    if (!docs) return;
    for (guint i = 0; i < docs->n_elms; i++) {
        MooEdit *doc = docs->elms[i];
        char *fn = moo_edit_get_filename (doc);
        gboolean match = fn && !strcmp (fn, bp->file);
        g_free (fn);
        if (!match) continue;
        MooEditView *view = moo_edit_get_view (doc);
        if (!view) continue;
        GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
        if (!MOO_IS_TEXT_BUFFER (buf)) continue;

        bp->mark = (MooLineMark *) g_object_new (MOO_TYPE_LINE_MARK,
            "visible", TRUE, (const char *) NULL);
        moo_line_mark_set_stock_id (bp->mark, "gtk-stop");
        /* line is 1-based in our model; MooTextBuffer expects 0-based */
        moo_text_buffer_add_line_mark (MOO_TEXT_BUFFER (buf),
                                       bp->mark, bp->line - 1);
        /* MooTextView's left gutter doesn't display marks unless
         * the show-line-marks property is on (the default is off).
         * Turn it on for the view we just dropped a mark in so the
         * gutter actually renders the red dot. */
        g_object_set (view, "show-line-marks", TRUE, NULL);
        break;
    }
    moo_edit_array_free (docs);
}

static void
detach_visual_mark (GdbBreakpoint *bp)
{
    if (!bp->mark) return;
    MooTextBuffer *buf = moo_line_mark_get_buffer (bp->mark);
    if (buf)
        moo_text_buffer_delete_line_mark (buf, bp->mark);
    g_object_unref (bp->mark);
    bp->mark = NULL;
}

/* Remove the current-line marker (if any).  Called when execution
 * resumes, when gdb quits, and before each new "stopped" event so we
 * always have at most one. */
static void
clear_exec_mark (MooGdbWin *win)
{
    if (win->exec_mark) {
        MooTextBuffer *buf = moo_line_mark_get_buffer (win->exec_mark);
        if (buf)
            moo_text_buffer_delete_line_mark (buf, win->exec_mark);
        g_object_unref (win->exec_mark);
        win->exec_mark = NULL;
    }
    g_free (win->exec_file);
    win->exec_file = NULL;
    win->exec_line = 0;
}

/* Place / replace the current-line marker at file:line.
 *
 * If `file` is already open in this window, focus its tab, place
 * the arrow, and scroll into view.
 *
 * If `file` isn't open: only auto-open it when it actually exists
 * on disk (and is a regular file, not a directory).  Hitting a stop
 * inside glibc's __libc_start_main reports a source path like
 * "nptl/libc_start_call_main.h" that the user doesn't have a copy
 * of; trying to open that throws a popup.  In that case we just
 * cache the (file, line) for later — execution will usually step
 * back into the user's own source on the next resume. */
static gboolean
file_is_readable (const char *file)
{
    if (!file || !*file) return FALSE;
    return g_file_test (file, G_FILE_TEST_IS_REGULAR);
}

static void
set_exec_mark (MooGdbWin *win, const char *file, int line)
{
    clear_exec_mark (win);
    if (!file || line <= 0) return;

    win->exec_file = g_strdup (file);
    win->exec_line = line;

    MooEditor *editor = moo_editor_instance ();
    MooEdit   *doc    = moo_editor_get_doc (editor, file);
    if (!doc) {
        /* Don't open files we can't verify exist — glibc internals,
         * generated headers, etc. reported by gdb at function-prologue
         * stops would otherwise pop a "Can't open" error dialog. */
        if (!file_is_readable (file)) {
            console_append (win,
                "stopped at ", "log");
            console_append (win, file, "log");
            char *suffix = g_strdup_printf (":%d (source not available)\n", line);
            console_append (win, suffix, "log");
            g_free (suffix);
            return;
        }
        moo_editor_open_path (editor, file, NULL, 0, win->window);
        doc = moo_editor_get_doc (editor, file);
    }
    if (!doc) return;

    MooEditView *view = moo_edit_get_view (doc);
    if (!view) return;
    GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    if (!MOO_IS_TEXT_BUFFER (buf)) return;

    win->exec_mark = (MooLineMark *) g_object_new (MOO_TYPE_LINE_MARK,
        "visible", TRUE, (const char *) NULL);
    moo_line_mark_set_stock_id (win->exec_mark, "gtk-go-forward");
    moo_text_buffer_add_line_mark (MOO_TEXT_BUFFER (buf),
                                   win->exec_mark, line - 1);
    g_object_set (view, "show-line-marks", TRUE, NULL);

    moo_edit_window_set_active_doc (win->window, doc);
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_line (buf, &iter, line - 1);
    gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (view),
                                  &iter, 0.0, TRUE, 0.0, 0.3);
}

/* Session "stopped" / "running" / "exited" handlers. */

static void
on_session_stopped (G_GNUC_UNUSED MooGdbSession *s,
                    G_GNUC_UNUSED const char *reason,
                    const char *file, int line,
                    G_GNUC_UNUSED const char *function,
                    gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    set_exec_mark (win, file, line);
    /* Attach the query-tooltip handler to whatever source views are
     * currently open — including ones loaded after the last stop. */
    attach_hover_to_open_docs (win);
}

static void
on_session_running (G_GNUC_UNUSED MooGdbSession *s, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    clear_exec_mark (win);
    /* Hover values are frame-local — drop the cache so the next
     * stop doesn't show stale values from before the resume. */
    if (win->hover_cache)   g_hash_table_remove_all (win->hover_cache);
    if (win->hover_pending) g_hash_table_remove_all (win->hover_pending);
}

static void drop_session (MooGdbWin *win);

static void
on_session_exited (G_GNUC_UNUSED MooGdbSession *s, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    clear_exec_mark (win);
    if (win->hover_cache)   g_hash_table_remove_all (win->hover_cache);
    if (win->hover_pending) g_hash_table_remove_all (win->hover_pending);
    /* gdb is gone; drop the dead session so the next Start spawns a new one
       instead of writing commands into a closed pipe. */
    drop_session (win);
}

/* Session "breakpoint-added" handler — gdb gave us a number; if we
 * have a pending placeholder at file:line, fill in its number and
 * register it in the by-number table for fast removal. */
static void
on_session_bp_added (G_GNUC_UNUSED MooGdbSession *s,
                     int number, const char *file, int line,
                     gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    GdbBreakpoint *bp = lookup_bp (win, file, line);
    if (!bp) {
        /* Created via gdb console / =breakpoint-created with no prior
         * UI request — synthesize a record so the gutter mark shows. */
        bp = g_new0 (GdbBreakpoint, 1);
        bp->number = number;
        bp->file   = g_strdup (file);
        bp->line   = line;
        register_bp (win, bp);
        attach_visual_mark (win, bp);
        return;
    }
    bp->number = number;
    g_hash_table_insert (win->bp_by_number, GINT_TO_POINTER (number), bp);
    attach_visual_mark (win, bp);
}

static void
on_session_bp_removed (G_GNUC_UNUSED MooGdbSession *s,
                       int number, gpointer user_data)
{
    MooGdbWin *win = (MooGdbWin *) user_data;
    GdbBreakpoint *bp = (GdbBreakpoint *)
        g_hash_table_lookup (win->bp_by_number, GINT_TO_POINTER (number));
    if (!bp) return;
    detach_visual_mark (bp);
    GHashTable *line_tbl = (GHashTable *)
        g_hash_table_lookup (win->bp_by_file, bp->file);
    g_hash_table_remove (win->bp_by_number, GINT_TO_POINTER (number));
    /* line_tbl's free_bp destructor frees the entry. */
    if (line_tbl)
        g_hash_table_remove (line_tbl, GINT_TO_POINTER (bp->line));
}

/* Lazy session create — used when the user toggles a breakpoint
 * before they've launched the debugger.  Just enough state to track
 * breakpoints; -exec-run + execution control comes from Phase 5. */
/* Tear the session down and forget it.
 *
 * moo_gdb_session_shutdown() must run before the unref: the session's async
 * read loop holds a reference to itself, which only cancellation releases.
 * Clearing win->session is what lets ensure_session() build a fresh one --
 * without it, Stop followed by Start reused a session whose gdb was gone and
 * wrote into a closed pipe. */
static void
drop_session (MooGdbWin *win)
{
    if (!win || !win->session) return;

    MooGdbSession *s = win->session;
    win->session = NULL;
    g_signal_handlers_disconnect_by_data (s, win);
    moo_gdb_session_shutdown (s);
    g_object_unref (s);
}

static MooGdbSession *
ensure_session (MooGdbWin *win)
{
    if (win->session) return win->session;
    win->session = moo_gdb_session_new ();
    /* Session signals are released automatically when the session
     * is unref'd in moo_gdb_win_free, so we don't need to track
     * handler ids individually. */
    g_signal_connect (win->session, "breakpoint-added",
                      G_CALLBACK (on_session_bp_added), win);
    g_signal_connect (win->session, "breakpoint-removed",
                      G_CALLBACK (on_session_bp_removed), win);
    g_signal_connect (win->session, "stopped",
                      G_CALLBACK (on_session_stopped), win);
    g_signal_connect (win->session, "running",
                      G_CALLBACK (on_session_running), win);
    g_signal_connect (win->session, "exited",
                      G_CALLBACK (on_session_exited), win);
    /* Funnel console / log / error stream records into the
     * bottom-pane transcript.  Errors used to pop a dialog; now
     * they're appended to the pane in a bold red tag, which is
     * less interrupting and survives across multiple errors in a
     * single command burst. */
    g_signal_connect (win->session, "console-output",
                      G_CALLBACK (on_console_signal), win);
    g_signal_connect (win->session, "log-output",
                      G_CALLBACK (on_log_signal), win);
    g_signal_connect (win->session, "target-output",
                      G_CALLBACK (on_target_signal), win);
    g_signal_connect (win->session, "error",
                      G_CALLBACK (on_console_error), win);
    g_signal_connect (win->session, "locals-changed",
                      G_CALLBACK (on_locals_changed), win);
    g_signal_connect (win->session, "frames-changed",
                      G_CALLBACK (on_frames_changed), win);
    g_signal_connect (win->session, "watches-changed",
                      G_CALLBACK (on_watches_changed), win);
    GError *err = NULL;
    if (!moo_gdb_session_start (win->session, NULL, &err)) {
        g_warning ("[gdb] failed to spawn gdb: %s",
                   err ? err->message : "(unknown)");
        if (err) g_error_free (err);
        g_object_unref (win->session);
        win->session = NULL;
    }
    return win->session;
}

void
moo_gdb_win_toggle_bp (MooGdbWin *win, const char *file, int line)
{
    g_return_if_fail (win != NULL && file != NULL && line > 0);
    GdbBreakpoint *existing = lookup_bp (win, file, line);
    if (existing) {
        if (existing->number >= 0 && win->session)
            moo_gdb_session_break_remove (win->session, existing->number);
        else {
            /* Not yet registered with gdb (race between toggle and
             * insert reply).  Just drop the local visual + record. */
            detach_visual_mark (existing);
            GHashTable *line_tbl = (GHashTable *)
                g_hash_table_lookup (win->bp_by_file, file);
            if (line_tbl)
                g_hash_table_remove (line_tbl, GINT_TO_POINTER (line));
        }
        return;
    }

    /* New breakpoint — placeholder entry so future on_session_bp_added
     * can find it by (file,line). */
    GdbBreakpoint *bp = g_new0 (GdbBreakpoint, 1);
    bp->number = -1;
    bp->file   = g_strdup (file);
    bp->line   = line;
    register_bp (win, bp);
    attach_visual_mark (win, bp);

    MooGdbSession *s = ensure_session (win);
    if (s)
        moo_gdb_session_break_add (s, file, line);
}

/* ── Build runner ────────────────────────────────────────────────── */

/* Forward decls — these helpers are defined alongside
 * moo_gdb_win_start below, but the build runner uses them too. */
static char *active_doc_path (MooGdbWin *win);
static char *resolve_field   (MooGdbWin *win, const char *raw,
                               const char *active_file);

typedef void (*BuildDoneCb) (MooGdbWin *win, gboolean success,
                              gpointer user_data);

/* Two independent async operations share this context: the line reader and
 * the subprocess wait.  Either can complete first -- a child can exit while
 * output is still buffered -- so it is reference counted rather than freed by
 * whichever finishes last.
 *
 * `win` is cleared by moo_gdb_win_free when the window is closed mid-build;
 * every callback must therefore test it before use. */
typedef struct _BuildCtx {
    int          refs;
    MooGdbWin   *win;         /* NULL once the window is gone */
    GSubprocess *proc;
    GDataInputStream *stream;
    char        *label;       /* config name, for log lines */
    BuildDoneCb  cb;
    gpointer     cb_data;
} BuildCtx;

static BuildCtx *
build_ctx_ref (BuildCtx *ctx)
{
    if (ctx) ctx->refs++;
    return ctx;
}

static void
build_ctx_unref (BuildCtx *ctx)
{
    if (!ctx) return;
    if (--ctx->refs > 0) return;
    if (ctx->win && ctx->win->build_ctx == ctx)
        ctx->win->build_ctx = NULL;
    if (ctx->stream) g_object_unref (ctx->stream);
    if (ctx->proc)   g_object_unref (ctx->proc);
    g_free (ctx->label);
    g_free (ctx);
}

static void read_build_line (BuildCtx *ctx);

static void
on_build_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BuildCtx *ctx = (BuildCtx *) user_data;
    gsize len = 0;
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish_utf8 (
        G_DATA_INPUT_STREAM (source), res, &len, &err);
    if (err) {
        /* Cancelled (subprocess detach, or the window closing) is benign —
         * just stop reading.  Other errors get surfaced. */
        if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && ctx->win) {
            char *m = g_strdup_printf (
                "[build] read error: %s\n", err->message);
            console_append (ctx->win, m, "error");
            g_free (m);
        }
        g_error_free (err);
        build_ctx_unref (ctx);
        return;
    }
    if (!line) {
        /* EOF — wait for exit code to fire on_build_exit. */
        build_ctx_unref (ctx);
        return;
    }
    if (ctx->win) {
        char *display = g_strdup_printf ("%s\n", line);
        console_append (ctx->win, display, "build");
        g_free (display);
    }
    g_free (line);
    read_build_line (ctx);
}

static void
read_build_line (BuildCtx *ctx)
{
    if (!ctx->win) {          /* window gone: stop reading, drop the ref */
        build_ctx_unref (ctx);
        return;
    }
    g_data_input_stream_read_line_async (
        ctx->stream, G_PRIORITY_DEFAULT,
        ctx->win->build_cancel, on_build_line, ctx);
}

static void
on_build_exit (GObject *source, GAsyncResult *res, gpointer user_data)
{
    BuildCtx *ctx = (BuildCtx *) user_data;
    GError *err = NULL;
    gboolean waited =
        g_subprocess_wait_finish (G_SUBPROCESS (source), res, &err);
    gint code = -1;
    if (waited)
        code = g_subprocess_get_exit_status (G_SUBPROCESS (source));
    if (err) g_error_free (err);

    gboolean success = (code == 0);

    /* The window may have been closed while the build ran; the subprocess
       wait still completes, so nothing below may touch it. */
    if (!ctx->win) {
        build_ctx_unref (ctx);
        return;
    }

    ctx->win->is_building = FALSE;

    char *msg;
    if (success)
        msg = g_strdup_printf (
            "[build] '%s' succeeded\n", ctx->label);
    else
        msg = g_strdup_printf (
            "[build] '%s' failed (exit code %d)\n", ctx->label, code);
    console_append (ctx->win, msg, success ? "build" : "error");
    g_free (msg);

    BuildDoneCb cb     = ctx->cb;
    gpointer    cb_ud  = ctx->cb_data;
    MooGdbWin  *win    = ctx->win;
    build_ctx_unref (ctx);
    if (cb) cb (win, success, cb_ud);
}

/* Spawn `cmd` (via `sh -c` so the user can write pipes / globs in
 * launch.json) in directory `cwd`.  Returns FALSE and logs an error
 * if the spawn itself failed; success means the subprocess is
 * running — the eventual exit lands in on_build_exit which fires
 * the callback. */
static gboolean
run_build (MooGdbWin *win, const char *cmd, const char *cwd,
            const char *label,
            BuildDoneCb cb, gpointer cb_data)
{
    if (win->is_building) {
        console_append (win,
            "[build] another build is still running\n", "error");
        if (cb) cb (win, FALSE, cb_data);
        return FALSE;
    }
    GSubprocessLauncher *launcher = g_subprocess_launcher_new (
        (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                          | G_SUBPROCESS_FLAGS_STDERR_MERGE));
    if (cwd && *cwd)
        g_subprocess_launcher_set_cwd (launcher, cwd);

    GError *err = NULL;
    GSubprocess *proc = g_subprocess_launcher_spawn (
        launcher, &err, "sh", "-c", cmd, NULL);
    g_object_unref (launcher);
    if (!proc) {
        char *m = g_strdup_printf (
            "[build] failed to spawn: %s\n",
            err ? err->message : "(unknown)");
        console_append (win, m, "error");
        g_free (m);
        if (err) g_error_free (err);
        if (cb) cb (win, FALSE, cb_data);
        return FALSE;
    }

    char *banner = g_strdup_printf (
        "[build] $ %s%s%s\n", cmd,
        (cwd && *cwd) ? "  # in " : "",
        (cwd && *cwd) ? cwd : "");
    console_append (win, banner, "build");
    g_free (banner);

    if (win->build_cancel) g_object_unref (win->build_cancel);
    win->build_cancel = g_cancellable_new ();

    BuildCtx *ctx = g_new0 (BuildCtx, 1);
    ctx->win     = win;
    ctx->proc    = proc;
    ctx->stream  = g_data_input_stream_new (
        g_subprocess_get_stdout_pipe (proc));
    ctx->label   = g_strdup (label);
    ctx->cb      = cb;
    ctx->cb_data = cb_data;
    ctx->refs    = 0;
    win->build_ctx = ctx;

    win->is_building = TRUE;
    /* One reference per outstanding async operation. */
    read_build_line (build_ctx_ref (ctx));
    g_subprocess_wait_async (proc, win->build_cancel,
                             on_build_exit, build_ctx_ref (ctx));
    return TRUE;
}

/* Walk the project tree (capped depth) checking whether any C/C++
 * source/header file has an mtime newer than `binary_mtime`.
 * Returns TRUE if a rebuild looks warranted. */
static gboolean
any_source_newer (const char *dir, gint64 binary_mtime, int depth)
{
    if (depth > 5) return FALSE;
    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d) return FALSE;
    const char *name;
    gboolean stale = FALSE;
    while (!stale && (name = g_dir_read_name (d))) {
        if (name[0] == '.') continue;       /* skip hidden / vcs */
        /* Skip common build / external trees so a big node_modules
         * doesn't dominate the walk time. */
        if (!strcmp (name, "build")        ||
            !strcmp (name, "build-md")     ||
            !strcmp (name, "build-rel")    ||
            !strcmp (name, "build-spell")  ||
            !strcmp (name, "node_modules") ||
            !strcmp (name, ".git"))
            continue;
        char *path = g_build_filename (dir, name, NULL);
        if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
            stale = any_source_newer (path, binary_mtime, depth + 1);
        } else if (g_str_has_suffix (name, ".c")   ||
                   g_str_has_suffix (name, ".cpp") ||
                   g_str_has_suffix (name, ".cc")  ||
                   g_str_has_suffix (name, ".cxx") ||
                   g_str_has_suffix (name, ".h")   ||
                   g_str_has_suffix (name, ".hpp")) {
            MgwStatBuf st;
            mgw_errno_t err = MGW_E_NOERROR;
            if (mgw_stat (path, &st, &err) == 0 &&
                st.mtime.value > binary_mtime)
                stale = TRUE;
        }
        g_free (path);
    }
    g_dir_close (d);
    return stale;
}

static gboolean
binary_is_stale (const char *binary, const char *root)
{
    if (!binary || !*binary) return FALSE;
    if (!g_file_test (binary, G_FILE_TEST_IS_REGULAR)) return TRUE;
    MgwStatBuf bs;
    mgw_errno_t err2 = MGW_E_NOERROR;
    if (mgw_stat (binary, &bs, &err2) != 0) return TRUE;
    if (!root || !*root) return FALSE;
    return any_source_newer (root, bs.mtime.value, 0);
}

/* The actual launch — split from moo_gdb_win_start so the build
 * runner can call it as a continuation after a successful build. */
static void do_start_now (MooGdbWin *win);

static void
on_prelaunch_done (MooGdbWin *win, gboolean success,
                    G_GNUC_UNUSED gpointer ud)
{
    if (success) do_start_now (win);
    else console_append (win,
        "[build] aborting launch because build failed\n", "error");
}

void
moo_gdb_win_build (MooGdbWin *win)
{
    g_return_if_fail (win != NULL);
    if (!win->project || win->active_config < 0) {
        console_append (win,
            "[build] no launch configuration selected\n", "error");
        return;
    }
    const MooGdbConfig *cfg = moo_gdb_project_config (win->project,
                                                       (guint) win->active_config);
    if (!cfg || !cfg->build_command || !*cfg->build_command) {
        console_append (win,
            "[build] active configuration has no build command\n",
            "error");
        return;
    }
    char *active_file = active_doc_path (win);
    char *cmd = moo_gdb_project_resolve (win->project,
                                           cfg->build_command, active_file);
    char *cwd = cfg->cwd && *cfg->cwd
                ? moo_gdb_project_resolve (win->project, cfg->cwd, active_file)
                : g_strdup (moo_gdb_project_root (win->project));
    run_build (win, cmd, cwd, cfg->name, NULL, NULL);
    g_free (cmd);
    g_free (cwd);
    g_free (active_file);
}

/* ── Execution control forwards ──────────────────────────────────── */

/* Get the active document's full path, or NULL if none / unsaved.
 * Used both for project re-discovery and for ${file} substitution.
 * Returned pointer is owned by caller (free with g_free). */
static char *
active_doc_path (MooGdbWin *win)
{
    MooEdit *doc = moo_edit_window_get_active_doc (win->window);
    if (!doc) return NULL;
    return moo_edit_get_filename (doc);   /* transfer-full */
}

/* Resolve a launch.json string field through the project's
 * substitution helper.  Returns NULL if `raw` is NULL/empty. */
static char *
resolve_field (MooGdbWin *win, const char *raw, const char *active_file)
{
    if (!raw || !*raw) return NULL;
    return moo_gdb_project_resolve (win->project, raw, active_file);
}

void
moo_gdb_win_start (MooGdbWin *win)
{
    g_return_if_fail (win != NULL);
    if (win->is_building) {
        console_append (win,
            "[build] still building; launch will not start until "
            "the build finishes\n", "log");
        return;
    }

    /* When a launch config is active and its binary is missing or
     * older than any source file, run the preLaunchTask first and
     * defer the actual launch to on_prelaunch_done.  Without a
     * project we skip the check entirely. */
    if (win->project && win->active_config >= 0) {
        const MooGdbConfig *cfg = moo_gdb_project_config (win->project,
                                            (guint) win->active_config);
        if (cfg && cfg->build_command && *cfg->build_command) {
            char *active_file = active_doc_path (win);
            char *bin = resolve_field (win, cfg->program, active_file);
            const char *root = moo_gdb_project_root (win->project);
            gboolean stale = binary_is_stale (bin, root);
            if (stale) {
                console_append (win,
                    bin && g_file_test (bin, G_FILE_TEST_IS_REGULAR)
                      ? "[build] source newer than binary — rebuilding...\n"
                      : "[build] binary missing — building...\n",
                    "build");
                char *cmd = moo_gdb_project_resolve (win->project,
                                cfg->build_command, active_file);
                char *cwd = cfg->cwd && *cfg->cwd
                              ? moo_gdb_project_resolve (win->project,
                                                          cfg->cwd, active_file)
                              : g_strdup (root);
                run_build (win, cmd, cwd, cfg->name,
                            on_prelaunch_done, NULL);
                g_free (cmd);
                g_free (cwd);
                g_free (bin);
                g_free (active_file);
                return;
            }
            g_free (bin);
            g_free (active_file);
        }
    }

    do_start_now (win);
}

static void
do_start_now (MooGdbWin *win)
{
    g_return_if_fail (win != NULL);
    MooGdbSession *s = ensure_session (win);
    if (!s) return;

    /* Pick the target binary.  Priority order:
     *   1. The active launch.json configuration (highest fidelity
     *      and the new default — users who set up a project want
     *      their selection honoured).
     *   2. Whatever the user set via "Configure Target".
     *   3. Heuristic: strip the extension from the active doc's
     *      filename (foo.c → ./foo) — works for trivial single-
     *      file builds.  Kept as a fallback so the editor is still
     *      useful without any project setup. */
    const MooGdbConfig *cfg = NULL;
    if (win->project && win->active_config >= 0)
        cfg = moo_gdb_project_config (win->project,
                                       (guint) win->active_config);

    char *active_file = active_doc_path (win);
    char *target      = NULL;
    char *cwd_resolved = NULL;
    char **argv_resolved = NULL;
    GHashTable *env_resolved = NULL;   /* char*name → char*value */

    if (cfg) {
        target       = resolve_field (win, cfg->program, active_file);
        cwd_resolved = resolve_field (win, cfg->cwd,     active_file);
        if (cfg->args) {
            guint n = g_strv_length (cfg->args);
            argv_resolved = g_new0 (char *, n + 1);
            for (guint i = 0; i < n; i++)
                argv_resolved[i] = moo_gdb_project_resolve (win->project,
                                    cfg->args[i], active_file);
        }
        if (cfg->environment && g_hash_table_size (cfg->environment) > 0) {
            env_resolved = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, g_free);
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init (&it, cfg->environment);
            while (g_hash_table_iter_next (&it, &k, &v))
                g_hash_table_insert (env_resolved,
                    g_strdup ((const char *) k),
                    moo_gdb_project_resolve (win->project,
                                              (const char *) v, active_file));
        }
        char *msg = g_strdup_printf (
            "Launch configuration: %s\n", cfg->name);
        console_append (win, msg, "log");
        g_free (msg);
    } else if (win->cfg_target && *win->cfg_target) {
        target = g_strdup (win->cfg_target);
    } else if (active_file) {
        char *dot = strrchr (active_file, '.');
        if (dot && dot > strrchr (active_file, '/'))
            target = g_strndup (active_file, dot - active_file);
        else
            target = g_strdup (active_file);
    }
    g_free (active_file);

    /* Verify the inferred target exists & is executable before
     * sending anything to gdb — otherwise -file-exec-and-symbols
     * fails silently and the user sees a cascade of "No symbol
     * table is loaded" / "The program is not being run" errors
     * with no clue where to look.  Loud, actionable error in the
     * console is much friendlier. */
    if (!target || !*target) {
        console_append (win,
            "Cannot start: no target binary configured.  "
            "Use Edit → Configure Debug Target... or save the active "
            "document so its filename can be used as the target.\n",
            "error");
        g_free (target);
        return;
    }
    if (!g_file_test (target, G_FILE_TEST_IS_REGULAR)) {
        char *msg = g_strdup_printf (
            "Cannot start: target binary `%s` does not exist.\n"
            "Either compile it (`gcc -g -O0 source.c -o %s`) or "
            "set a different binary via Edit → Configure Debug Target...\n",
            target, target);
        console_append (win, msg, "error");
        g_free (msg);
        g_free (target);
        return;
    }
    if (!g_file_test (target, G_FILE_TEST_IS_EXECUTABLE)) {
        char *msg = g_strdup_printf (
            "Warning: target `%s` is not marked executable — "
            "gdb may refuse to run it.\n", target);
        console_append (win, msg, "error");
    }

    /* Log the actual target so the user can verify the heuristic
     * picked the right binary. */
    {
        char *msg = g_strdup_printf ("Loading target: %s\n", target);
        console_append (win, msg, "log");
        g_free (msg);
    }
    moo_gdb_session_set_target (s, target);
    g_free (target);

    /* Apply cwd: launch.json wins, then the legacy dialog field. */
    if (cwd_resolved && *cwd_resolved)
        moo_gdb_session_set_cwd (s, cwd_resolved);
    else if (win->cfg_cwd && *win->cfg_cwd)
        moo_gdb_session_set_cwd (s, win->cfg_cwd);
    g_free (cwd_resolved);

    /* Apply argv.  When a launch config is active, argv_resolved is
     * already a NULL-terminated array of resolved strings — pass it
     * verbatim.  Otherwise fall back to shell-parsing the legacy
     * dialog field. */
    if (argv_resolved) {
        moo_gdb_session_set_args (s, (const char *const *) argv_resolved);
        g_strfreev (argv_resolved);
    } else if (win->cfg_args && *win->cfg_args) {
        GError *err = NULL;
        char **argv = NULL;
        int    argc = 0;
        if (g_shell_parse_argv (win->cfg_args, &argc, &argv, &err)) {
            moo_gdb_session_set_args (s, (const char *const *) argv);
            g_strfreev (argv);
        } else {
            console_append (win,
                "warning: couldn't parse program arguments: ", "log");
            console_append (win, err->message, "error");
            console_append (win, "\n", "error");
            g_error_free (err);
        }
    } else {
        /* Clear any previously-set args so a stale list doesn't
         * carry between runs. */
        moo_gdb_session_set_args (s, NULL);
    }

    /* Environment variables from the launch config.  Pushed to gdb
     * via repeated `-gdb-set environment KEY=VALUE` so they're
     * picked up by `-exec-run`. */
    if (env_resolved) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init (&it, env_resolved);
        while (g_hash_table_iter_next (&it, &k, &v)) {
            char *cmd = g_strdup_printf (
                "-gdb-set environment %s=%s",
                (const char *) k, (const char *) v);
            moo_gdb_session_send_raw (s, cmd);
            g_free (cmd);
        }
        g_hash_table_destroy (env_resolved);
    }

    moo_gdb_session_run (s);
}

void
moo_gdb_win_toggle_bp_at_cursor (MooGdbWin *win)
{
    g_return_if_fail (win != NULL);
    MooEdit *doc = moo_edit_window_get_active_doc (win->window);
    if (!doc) return;
    char *file = moo_edit_get_filename (doc);
    if (!file) return;
    MooEditView *view = moo_edit_get_view (doc);
    if (!view) { g_free (file); return; }
    GtkTextBuffer *buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    GtkTextIter iter;
    gtk_text_buffer_get_iter_at_mark (buf, &iter,
        gtk_text_buffer_get_insert (buf));
    int line = gtk_text_iter_get_line (&iter) + 1;
    moo_gdb_win_toggle_bp (win, file, line);
    g_free (file);
}

void moo_gdb_win_continue (MooGdbWin *win)
    { if (win && win->session) moo_gdb_session_continue   (win->session); }
void moo_gdb_win_step_over (MooGdbWin *win)
    { if (win && win->session) moo_gdb_session_step_over  (win->session); }
void moo_gdb_win_step_into (MooGdbWin *win)
    { if (win && win->session) moo_gdb_session_step_into  (win->session); }
void moo_gdb_win_step_out  (MooGdbWin *win)
    { if (win && win->session) moo_gdb_session_step_out   (win->session); }
void moo_gdb_win_pause     (MooGdbWin *win)
    { if (win && win->session) moo_gdb_session_pause      (win->session); }
void moo_gdb_win_stop      (MooGdbWin *win)
    { if (win && win->session) { moo_gdb_session_quit (win->session);
                                 drop_session (win); } }

/* ── Configure-target dialog ─────────────────────────────────────── */

/* Helper: build a labelled-entry row "label: [entry] [browse?]" and
 * pack it into `grid` at `row`.  Returns the entry so the caller
 * can stash a pointer for later read-back. */
static GtkEntry *
add_labelled_entry (GtkGrid *grid, int row,
                    const char *label_text, const char *initial,
                    gboolean with_browse)
{
    GtkWidget *label = gtk_label_new (label_text);
    gtk_label_set_xalign (GTK_LABEL (label), 1.0);
    gtk_widget_set_hexpand (label, FALSE);
    gtk_grid_attach (grid, label, 0, row, 1, 1);

    GtkWidget *entry = gtk_entry_new ();
    if (initial) gtk_entry_set_text (GTK_ENTRY (entry), initial);
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_entry_set_width_chars (GTK_ENTRY (entry), 40);
    gtk_grid_attach (grid, entry, 1, row, 1, 1);
    (void) with_browse;   /* file-chooser button reserved for later */
    return GTK_ENTRY (entry);
}

void
moo_gdb_win_configure (MooGdbWin *win)
{
    g_return_if_fail (win != NULL);

    /* When a project is loaded, "Configure" means "edit launch.json".
     * Open it in medit so the user can edit the same source of truth
     * VS Code uses.  Fall through to the legacy dialog only when no
     * project file was discovered. */
    if (win->project) {
        const char *p = moo_gdb_project_launch_path (win->project);
        if (p && *p) {
            moo_editor_open_path (moo_editor_instance (), p, NULL, 0,
                                   win->window);
            return;
        }
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons (
        _("Configure Debug Target"),
        GTK_WINDOW (win->window),
        (GtkDialogFlags) (GTK_DIALOG_DESTROY_WITH_PARENT
                          | GTK_DIALOG_MODAL),
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("_OK"),     GTK_RESPONSE_OK,
        (const char *) NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_OK);

    GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
    GtkWidget *grid    = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
    g_object_set (grid, "margin", 12, NULL);
    gtk_box_pack_start (GTK_BOX (content), grid, TRUE, TRUE, 0);

    GtkEntry *target_entry = add_labelled_entry (GTK_GRID (grid), 0,
        _("Target binary:"), win->cfg_target, TRUE);
    gtk_entry_set_placeholder_text (target_entry,
        _("e.g. ./a.out — leave empty to auto-derive from current source"));

    GtkEntry *args_entry = add_labelled_entry (GTK_GRID (grid), 1,
        _("Program arguments:"), win->cfg_args, FALSE);
    gtk_entry_set_placeholder_text (args_entry,
        _("Shell-style: my_arg \"quoted with space\" --verbose"));

    GtkEntry *cwd_entry = add_labelled_entry (GTK_GRID (grid), 2,
        _("Working directory:"), win->cfg_cwd, TRUE);
    gtk_entry_set_placeholder_text (cwd_entry,
        _("Empty: inherit medit's cwd"));

    gtk_widget_show_all (dlg);

    if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_OK)
    {
        g_free (win->cfg_target);
        g_free (win->cfg_args);
        g_free (win->cfg_cwd);
        win->cfg_target = g_strdup (gtk_entry_get_text (target_entry));
        win->cfg_args   = g_strdup (gtk_entry_get_text (args_entry));
        win->cfg_cwd    = g_strdup (gtk_entry_get_text (cwd_entry));
        console_append (win, "Debug target updated.\n", "log");
    }
    gtk_widget_destroy (dlg);
}

MooGdbWin *
moo_gdb_win_new (MooEditWindow *window)
{
    MooGdbWin *win = g_new0 (MooGdbWin, 1);
    win->window      = window;
    win->bp_by_file  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, free_file_table);
    win->bp_by_number = g_hash_table_new (g_direct_hash, g_direct_equal);
    win->hover_views  = g_ptr_array_new ();
    win->hover_cache  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
    win->hover_pending = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
    win->active_config = -1;

    /* Build the console transcript pane up-front so the user sees
     * a greeting line even before they start a debug session. */
    build_console_pane (win);
    console_append (win,
        "GDB Console — type a command and press Enter, "
        "or use Ctrl+F5 to start debugging.\n", "log");

    /* Combined right-side "Debugger" pane stacks Locals + Stack +
     * Watches vertically as three resizable sections.  Each section
     * still auto-populates via the matching session signal
     * (locals-changed / frames-changed / watches-changed) — only
     * their layout is shared. */
    build_inspect_pane (win);

    /* Listen for active-doc changes so the project model can re-
     * discover whenever the user switches to a file in a different
     * tree.  Also kick off a one-shot discover now so the dropdown
     * is populated by the time the user looks at it. */
    win->active_doc_handler = g_signal_connect (window,
        "notify::active-doc",
        G_CALLBACK (on_active_doc_notify), win);
    win->new_doc_handler = g_signal_connect (window,
        "new-doc",
        G_CALLBACK (on_new_doc), win);
    reload_project (win);

    /* Note: gutter-click breakpoint toggling and doc-loaded
     * re-attach were tried via an emission hook on
     * line-mark-clicked + a connect on a doc-loaded editor signal,
     * but the signal lookup races plugin init (the class isn't
     * always initialized yet) and there's no doc-loaded signal on
     * MooEditor.  The F9 menu action covers the common case for
     * now; per-view connection on a doc-show signal can be added
     * later when those concerns are sorted out. */
    return win;
}

void
moo_gdb_win_free (MooGdbWin *win)
{
    if (!win) return;

    if (win->console_pane)
        moo_edit_window_remove_pane (win->window, MOO_GDB_CONSOLE_PANE_ID);
    /* Locals / Stack / Watches share a single combined pane — one
     * removal tears down all three at once. */
    if (win->locals_pane || win->frames_pane || win->watches_pane)
        moo_edit_window_remove_pane (win->window, MOO_GDB_INSPECT_PANE_ID);

    clear_exec_mark (win);
    detach_hover_from_views (win);

    drop_session (win);
    if (win->active_doc_handler)
        g_signal_handler_disconnect (win->window, win->active_doc_handler);
    if (win->new_doc_handler)
        g_signal_handler_disconnect (win->window, win->new_doc_handler);
    if (win->build_cancel) {
        g_cancellable_cancel (win->build_cancel);
        g_object_unref (win->build_cancel);
        win->build_cancel = NULL;
    }
    /* Cancelling completes the wait and the read rather than aborting them,
       so both callbacks still run.  Disown the context so they do not write
       through a freed window. */
    if (win->build_ctx) {
        win->build_ctx->win = NULL;
        win->build_ctx = NULL;
    }
    if (win->project) moo_gdb_project_free (win->project);

    g_hash_table_destroy (win->bp_by_file);
    g_hash_table_destroy (win->bp_by_number);
    if (win->hover_cache)   g_hash_table_destroy (win->hover_cache);
    if (win->hover_pending) g_hash_table_destroy (win->hover_pending);
    g_free (win->cfg_target);
    g_free (win->cfg_args);
    g_free (win->cfg_cwd);
    g_free (win);
}
