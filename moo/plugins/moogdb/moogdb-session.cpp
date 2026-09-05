/*
 *   moogdb-session.cpp
 *
 *   Phase 1 implementation of MooGdbSession.  Spawns a gdb
 *   subprocess in MI mode, reads lines asynchronously, dispatches
 *   MI records, and supports a single round-trip command
 *   (`-gdb-version`).  Execution control and breakpoints land in
 *   later commits.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#include "plugins/moogdb/moogdb-session.h"
#include "plugins/moogdb/moogdb-mi-parser.h"

#include <gio/gio.h>
#include <string.h>

/* ── Object layout ────────────────────────────────────────────────── */

struct _MooGdbSession {
    GObject parent;

    GSubprocess      *gdb;
    GOutputStream    *gdb_in;
    GDataInputStream *gdb_out;
    GCancellable     *cancellable;

    MooGdbState       state;
    char             *version;

    /* Monotonic command counter.  Each outgoing -command is prefixed
     * with `<token>` so the matching `<token>^done,...` response can
     * be routed back to the issuer. */
    guint             next_token;
    GHashTable       *pending;   /* token (GINT) → ResponseCB struct */

    /* Most-recent variable snapshot, refreshed automatically after
     * every *stopped event.  Each element is a MooGdbLocal*; freed
     * with the session. */
    GPtrArray        *locals;

    /* Most-recent stack frames, refreshed automatically after every
     * *stopped event.  Each element is a MooGdbFrame*. */
    GPtrArray        *frames;

    /* User-defined watch expressions.  Each element is a
     * MooGdbWatch* (or NULL for a freed slot).  Re-evaluated on
     * every *stopped via -data-evaluate-expression per slot. */
    GPtrArray        *watches;

    /* Names of varobjs we've asked gdb to create via -var-create.
     * Cleared on every *running (and on quit) by sending -var-delete
     * for each name — varobjs are frame-bound, so they're stale by
     * the time the next *stopped lands. */
    GPtrArray        *varobjs;

    /* TRUE while an inferior process exists and has a live frame to
     * inspect.  Set on *running; cleared on *stopped reasons whose
     * name starts with "exited" (exited, exited-normally, exited-
     * signalled).  The session state stays MOO_GDB_STATE_STOPPED in
     * that case because gdb itself is still alive — this flag is the
     * finer-grained distinction the hover-eval path needs to suppress
     * `-data-evaluate-expression` calls that would otherwise come
     * back as the noisy "No registers." error. */
    gboolean          inferior_alive;
};

G_DEFINE_TYPE (MooGdbSession, moo_gdb_session, G_TYPE_OBJECT)

typedef void (*ResponseCB) (MooGdbSession *s,
                            MooGdbMiRecord *record,
                            gpointer        user_data);

typedef struct {
    ResponseCB cb;
    gpointer   user_data;
} PendingEntry;

/* ── Signals ──────────────────────────────────────────────────────── */

enum {
    SIG_STATE_CHANGED,
    SIG_CONSOLE_OUTPUT,
    SIG_LOG_OUTPUT,
    SIG_TARGET_OUTPUT,
    SIG_RUNNING,
    SIG_STOPPED,
    SIG_BP_ADDED,
    SIG_BP_REMOVED,
    SIG_ERROR,
    SIG_LOCALS_CHANGED,
    SIG_FRAMES_CHANGED,
    SIG_WATCHES_CHANGED,
    SIG_EXITED,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* ── Forwards ─────────────────────────────────────────────────────── */

static void  start_read_loop  (MooGdbSession *s);
static void  on_line_async    (GObject *source, GAsyncResult *res, gpointer user_data);
static void  dispatch_record  (MooGdbSession *s, MooGdbMiRecord *r);
static void  send_command     (MooGdbSession *s,
                               const char    *cmd,
                               ResponseCB     cb,
                               gpointer       user_data);
static void  set_state        (MooGdbSession *s, MooGdbState st);
static void  on_version_reply (MooGdbSession *s,
                               MooGdbMiRecord *r,
                               gpointer        user_data);
static void  emit_bp_added_from_bkpt (MooGdbSession *s,
                                       MooGdbMiValue *bkpt_val);
static void  request_locals_refresh   (MooGdbSession *s);
static void  on_locals_reply          (MooGdbSession *s,
                                        MooGdbMiRecord *r,
                                        gpointer        user_data);
static void  request_frames_refresh   (MooGdbSession *s);
static void  on_frames_reply          (MooGdbSession *s,
                                        MooGdbMiRecord *r,
                                        gpointer        user_data);
static void  request_watches_refresh  (MooGdbSession *s);
static void  on_watch_reply           (MooGdbSession *s,
                                        MooGdbMiRecord *r,
                                        gpointer        user_data);

/* ── Life-cycle ───────────────────────────────────────────────────── */

static void
free_local (gpointer p)
{
    MooGdbLocal *l = (MooGdbLocal *) p;
    if (!l) return;
    g_free (l->name);
    g_free (l->type);
    g_free (l->value);
    g_free (l);
}

static void
free_frame (gpointer p)
{
    MooGdbFrame *f = (MooGdbFrame *) p;
    if (!f) return;
    g_free (f->function);
    g_free (f->file);
    g_free (f->addr);
    g_free (f);
}

static void
free_watch (gpointer p)
{
    MooGdbWatch *w = (MooGdbWatch *) p;
    if (!w) return;
    g_free (w->expression);
    g_free (w->value);
    g_free (w);
}

static void
moo_gdb_session_init (MooGdbSession *s)
{
    s->state       = MOO_GDB_STATE_IDLE;
    s->next_token  = 1;
    s->pending     = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                             NULL, g_free);
    s->cancellable = g_cancellable_new ();
    s->locals      = g_ptr_array_new_with_free_func (free_local);
    s->frames      = g_ptr_array_new_with_free_func (free_frame);
    s->watches     = g_ptr_array_new_with_free_func (free_watch);
    s->varobjs     = g_ptr_array_new_with_free_func (g_free);
}

static void
moo_gdb_session_finalize (GObject *object)
{
    MooGdbSession *s = MOO_GDB_SESSION (object);
    /* If gdb is still alive, force it down — quit() does a polite
     * "-gdb-exit" but at finalize we may not have time to wait. */
    if (s->gdb) {
        g_cancellable_cancel (s->cancellable);
        g_subprocess_force_exit (s->gdb);
        g_object_unref (s->gdb);
        s->gdb = NULL;
    }
    g_clear_object (&s->gdb_out);
    g_clear_object (&s->cancellable);
    /* gdb_in is owned by gdb; don't unref. */
    g_hash_table_destroy (s->pending);
    if (s->locals)  g_ptr_array_free (s->locals,  TRUE);
    if (s->frames)  g_ptr_array_free (s->frames,  TRUE);
    if (s->watches) g_ptr_array_free (s->watches, TRUE);
    if (s->varobjs) g_ptr_array_free (s->varobjs, TRUE);
    g_free (s->version);
    G_OBJECT_CLASS (moo_gdb_session_parent_class)->finalize (object);
}

GPtrArray *
moo_gdb_session_get_locals (MooGdbSession *s)
{
    g_return_val_if_fail (MOO_IS_GDB_SESSION (s), NULL);
    return s->locals;
}

GPtrArray *
moo_gdb_session_get_frames (MooGdbSession *s)
{
    g_return_val_if_fail (MOO_IS_GDB_SESSION (s), NULL);
    return s->frames;
}

GPtrArray *
moo_gdb_session_get_watches (MooGdbSession *s)
{
    g_return_val_if_fail (MOO_IS_GDB_SESSION (s), NULL);
    return s->watches;
}

guint
moo_gdb_session_add_watch (MooGdbSession *s, const char *expr)
{
    g_return_val_if_fail (MOO_IS_GDB_SESSION (s), 0);
    g_return_val_if_fail (expr != NULL && *expr, 0);
    MooGdbWatch *w = g_new0 (MooGdbWatch, 1);
    w->expression = g_strdup (expr);
    /* Reuse the first NULL slot if there is one (preserves slot
     * indices that may already be referenced elsewhere); otherwise
     * append. */
    guint slot;
    for (slot = 0; slot < s->watches->len; slot++)
        if (!s->watches->pdata[slot]) {
            s->watches->pdata[slot] = w;
            return slot;
        }
    g_ptr_array_add (s->watches, w);
    /* Trigger an evaluation immediately if we're stopped — the user
     * just typed it, they want to see the value. */
    if (s->state == MOO_GDB_STATE_STOPPED)
        moo_gdb_session_eval_watch (s, slot, expr);
    return slot;
}

void
moo_gdb_session_remove_watch (MooGdbSession *s, guint slot)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (slot >= s->watches->len) return;
    /* Don't shrink the array — free the slot in place so other
     * existing slot indices stay valid. */
    free_watch (s->watches->pdata[slot]);
    s->watches->pdata[slot] = NULL;
    g_signal_emit (s, signals[SIG_WATCHES_CHANGED], 0);
}

void
moo_gdb_session_eval_watch (MooGdbSession *s, guint slot, const char *expr)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!expr || !*expr) return;
    /* Stash the slot in the command callback's user_data so the
     * reply handler can write the result back into the right
     * slot.  GINT_TO_POINTER is fine for slot indices (small). */
    char *esc = g_strescape (expr, "");
    char *cmd = g_strdup_printf ("-data-evaluate-expression \"%s\"", esc);
    g_free (esc);
    /* The pending-table value is the callback's user_data, not the
     * callback's own state — but our send_command's PendingEntry
     * has user_data wired in.  We pass slot via the user_data
     * pointer.  + 1 so we can distinguish "slot 0" from NULL. */
    send_command (s, cmd, on_watch_reply,
                  GUINT_TO_POINTER (slot + 1));
    g_free (cmd);
}

/* One-shot expression evaluator used by the source-view hover
 * tooltip.  Allocates a context to carry the callback and the
 * original expression through send_command's reply path, then
 * trampolines to the user callback when the reply lands. */
typedef struct {
    MooGdbEvalCb cb;
    gpointer     user_data;
    char        *expr;
} EvalCtx;

static void
on_eval_async_reply (MooGdbSession *s, MooGdbMiRecord *r, gpointer user_data)
{
    EvalCtx *ctx = (EvalCtx *) user_data;
    const char *value    = NULL;
    gboolean    is_error = FALSE;
    const char *klass    = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done")) {
        MooGdbMiValue *v = moo_gdb_mi_record_field (r, "value");
        if (v) value = moo_gdb_mi_value_string (v);
    } else if (klass && !strcmp (klass, "error")) {
        MooGdbMiValue *m = moo_gdb_mi_record_field (r, "msg");
        if (m) value = moo_gdb_mi_value_string (m);
        is_error = TRUE;
    }
    if (ctx->cb)
        ctx->cb (s, ctx->expr, value, is_error, ctx->user_data);
    g_free (ctx->expr);
    g_free (ctx);
}

void
moo_gdb_session_eval_async (MooGdbSession *s, const char *expr,
                            MooGdbEvalCb cb, gpointer user_data)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!expr || !*expr) return;
    /* Sync-fail if there's no live frame to evaluate in — saves the
     * caller a "did the session stop?" check before every hover.
     * The inferior_alive guard catches the case where the program
     * exited but the session is still in STATE_STOPPED (gdb itself
     * lives on); without it, every hover would round-trip to gdb
     * and come back as the noisy "No registers." error. */
    if (s->state != MOO_GDB_STATE_STOPPED || !s->inferior_alive) {
        if (cb) cb (s, expr, NULL, TRUE, user_data);
        return;
    }
    EvalCtx *ctx = g_new0 (EvalCtx, 1);
    ctx->cb        = cb;
    ctx->user_data = user_data;
    ctx->expr      = g_strdup (expr);
    char *esc = g_strescape (expr, "");
    char *cmd = g_strdup_printf ("-data-evaluate-expression \"%s\"", esc);
    g_free (esc);
    send_command (s, cmd, on_eval_async_reply, ctx);
    g_free (cmd);
}

/* ── GDB variable objects ────────────────────────────────────────── */

typedef struct {
    char *expr;
    MooGdbVarCreateCb cb;
    gpointer user_data;
} VarCreateCtx;

static void
on_var_create_reply (MooGdbSession *s, MooGdbMiRecord *r, gpointer user_data)
{
    VarCreateCtx *ctx = (VarCreateCtx *) user_data;
    const char *klass = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done")) {
        MooGdbMiValue *name_v = moo_gdb_mi_record_field (r, "name");
        MooGdbMiValue *type_v = moo_gdb_mi_record_field (r, "type");
        MooGdbMiValue *val_v  = moo_gdb_mi_record_field (r, "value");
        MooGdbMiValue *nc_v   = moo_gdb_mi_record_field (r, "numchild");
        const char *name = name_v ? moo_gdb_mi_value_string (name_v) : NULL;
        const char *type = type_v ? moo_gdb_mi_value_string (type_v) : NULL;
        const char *val  = val_v  ? moo_gdb_mi_value_string (val_v)  : NULL;
        const char *nc_s = nc_v ? moo_gdb_mi_value_string (nc_v) : NULL;
        int nc = nc_s ? atoi (nc_s) : 0;
        if (name)
            g_ptr_array_add (s->varobjs, g_strdup (name));
        if (ctx->cb)
            ctx->cb (s, ctx->expr, name, type, val, nc, FALSE, ctx->user_data);
    } else {
        const char *msg = NULL;
        if (klass && !strcmp (klass, "error")) {
            MooGdbMiValue *m = moo_gdb_mi_record_field (r, "msg");
            if (m) msg = moo_gdb_mi_value_string (m);
        }
        if (ctx->cb)
            ctx->cb (s, ctx->expr, NULL, NULL, msg, 0, TRUE, ctx->user_data);
    }
    g_free (ctx->expr);
    g_free (ctx);
}

void
moo_gdb_session_var_create (MooGdbSession *s, const char *expr,
                            MooGdbVarCreateCb cb, gpointer user_data)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!expr || !*expr) {
        if (cb) cb (s, expr, NULL, NULL, NULL, 0, TRUE, user_data);
        return;
    }
    if (s->state != MOO_GDB_STATE_STOPPED) {
        if (cb) cb (s, expr, NULL, NULL, NULL, 0, TRUE, user_data);
        return;
    }
    VarCreateCtx *ctx = g_new0 (VarCreateCtx, 1);
    ctx->expr = g_strdup (expr);
    ctx->cb        = cb;
    ctx->user_data = user_data;
    /* `-var-create - * EXPR` — `-` lets gdb pick the name, `*` binds to the
     * current frame.  EXPR reaches here from the Watches pane as well as from
     * gdb itself, so it is escaped like any other quoted MI argument. */
    char *esc = g_strescape (expr, "");
    char *cmd = g_strdup_printf ("-var-create - * \"%s\"", esc);
    g_free (esc);
    send_command (s, cmd, on_var_create_reply, ctx);
    g_free (cmd);
}

static void
free_var_child (gpointer p)
{
    MooGdbVarChild *c = (MooGdbVarChild *) p;
    if (!c) return;
    g_free (c->name);
    g_free (c->exp);
    g_free (c->type);
    g_free (c->value);
    g_free (c);
}

typedef struct {
    char *parent;
    MooGdbVarChildrenCb cb;
    gpointer user_data;
} VarChildrenCtx;

static void
on_var_children_reply (MooGdbSession *s, MooGdbMiRecord *r, gpointer user_data)
{
    VarChildrenCtx *ctx = (VarChildrenCtx *) user_data;
    GPtrArray *children = g_ptr_array_new_with_free_func (free_var_child);
    gboolean is_error = FALSE;
    const char *klass = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done")) {
        MooGdbMiValue *kids = moo_gdb_mi_record_field (r, "children");
        if (kids) {
            guint n = moo_gdb_mi_value_list_len (kids);
            for (guint i = 0; i < n; i++) {
                MooGdbMiValue *child = moo_gdb_mi_value_list_nth (kids, i);
                if (!child) continue;
                MooGdbVarChild *c = g_new0 (MooGdbVarChild, 1);
                MooGdbMiValue *v;
                v = moo_gdb_mi_value_tuple_get (child, "name");
                if (v) c->name = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (child, "exp");
                if (v) c->exp = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (child, "type");
                if (v) c->type = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (child, "value");
                if (v) c->value = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (child, "numchild");
                if (v) {
                    const char *nc_s = moo_gdb_mi_value_string (v);
                    c->numchild = nc_s ? atoi (nc_s) : 0;
                }
                /* Track the gdb-side varobj so we can delete it on
                 * the next *running. */
                if (c->name)
                    g_ptr_array_add (s->varobjs, g_strdup (c->name));
                g_ptr_array_add (children, c);
            }
        }
    } else {
        is_error = TRUE;
    }
    if (ctx->cb)
        ctx->cb (s, ctx->parent, children, is_error, ctx->user_data);
    g_ptr_array_unref (children);
    g_free (ctx->parent);
    g_free (ctx);
}

void
moo_gdb_session_var_children (MooGdbSession *s, const char *varobj_name,
                              MooGdbVarChildrenCb cb, gpointer user_data)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!varobj_name || !*varobj_name) {
        if (cb) cb (s, varobj_name, NULL, TRUE, user_data);
        return;
    }
    VarChildrenCtx *ctx = g_new0 (VarChildrenCtx, 1);
    ctx->parent    = g_strdup (varobj_name);
    ctx->cb        = cb;
    ctx->user_data = user_data;
    /* `--simple-values` returns inline scalar values + types but
     * skips composite ones — matching the locals-pane convention so
     * the user can drill into nested aggregates with one click each. */
    char *cmd = g_strdup_printf (
        "-var-list-children --simple-values %s", varobj_name);
    send_command (s, cmd, on_var_children_reply, ctx);
    g_free (cmd);
}

void
moo_gdb_session_select_frame (MooGdbSession *s, int level)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    g_return_if_fail (level >= 0);
    /* Switch the selected frame, then re-pull locals at the new
     * frame so the Locals pane refreshes.  -stack-select-frame
     * doesn't auto-refresh anything — gdb just remembers which
     * frame later commands target. */
    char *cmd = g_strdup_printf ("-stack-select-frame %d", level);
    send_command (s, cmd, NULL, NULL);
    g_free (cmd);
    request_locals_refresh (s);
}

static void
moo_gdb_session_class_init (MooGdbSessionClass *klass)
{
    GObjectClass *go = G_OBJECT_CLASS (klass);
    go->finalize = moo_gdb_session_finalize;

    signals[SIG_STATE_CHANGED] = g_signal_new (
        "state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);

    signals[SIG_CONSOLE_OUTPUT] = g_signal_new (
        "console-output", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIG_LOG_OUTPUT] = g_signal_new (
        "log-output", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    /* "target-output" :: (const char *line)
     * Fires for @"..." MI records — the inferior's stdout/stderr
     * as captured by gdb.  Surface this in the console pane so the
     * user can see their program's printf() output. */
    signals[SIG_TARGET_OUTPUT] = g_signal_new (
        "target-output", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIG_RUNNING] = g_signal_new (
        "running", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);

    /* "stopped" carries the file/line/function gdb reported via the
     * *stopped record's `frame` field, plus the textual stop reason
     * (breakpoint-hit, end-stepping-range, exited-normally, ...).
     * Strings are borrowed; copy them in the handler if needed. */
    signals[SIG_STOPPED] = g_signal_new (
        "stopped", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 4,
        G_TYPE_STRING,    /* reason  */
        G_TYPE_STRING,    /* file    (canonical path, may be NULL) */
        G_TYPE_INT,       /* line    (1-based, 0 if unknown) */
        G_TYPE_STRING);   /* function (may be NULL) */

    /* "breakpoint-added" :: (int number, const char *file, int line)
     * Fires for both the synchronous reply to -break-insert and the
     * async =breakpoint-created notification (gdb sends both for
     * the same insert; consumers should dedupe by number). */
    signals[SIG_BP_ADDED] = g_signal_new (
        "breakpoint-added", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT);

    signals[SIG_BP_REMOVED] = g_signal_new (
        "breakpoint-removed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__INT,
        G_TYPE_NONE, 1, G_TYPE_INT);

    /* "locals-changed" :: ()
     * Fires after a *stopped event once the auto-issued
     * -stack-list-variables reply has been parsed.  Consumers
     * call moo_gdb_session_get_locals to read the new snapshot. */
    signals[SIG_LOCALS_CHANGED] = g_signal_new (
        "locals-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);

    /* "frames-changed" :: () — same shape, for the stack frames
     * snapshot refreshed by -stack-list-frames. */
    signals[SIG_FRAMES_CHANGED] = g_signal_new (
        "frames-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);

    /* "watches-changed" :: () — same shape, for watch-expression
     * results refreshed by -data-evaluate-expression. */
    signals[SIG_WATCHES_CHANGED] = g_signal_new (
        "watches-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);

    /* "error" :: (const char *msg)
     * Fired when gdb responds with `^error,msg="..."`.  Useful for
     * surfacing problems like "No such file or directory" from a
     * -file-exec-and-symbols, or "function not defined" from a
     * -break-insert.  Stay tolerant — error is per-command, not a
     * session-fatal state. */
    signals[SIG_ERROR] = g_signal_new (
        "error", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
        G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIG_EXITED] = g_signal_new (
        "exited", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);
}

MooGdbSession *
moo_gdb_session_new (void)
{
    return MOO_GDB_SESSION (g_object_new (MOO_TYPE_GDB_SESSION, NULL));
}

/* ── State ────────────────────────────────────────────────────────── */

static void
set_state (MooGdbSession *s, MooGdbState st)
{
    if (s->state == st) return;
    s->state = st;
    g_signal_emit (s, signals[SIG_STATE_CHANGED], 0);
}

MooGdbState moo_gdb_session_get_state   (MooGdbSession *s) { return s->state; }
gboolean    moo_gdb_session_inferior_alive (MooGdbSession *s)
    { return s ? s->inferior_alive : FALSE; }
const char *moo_gdb_session_get_version (MooGdbSession *s) { return s->version; }

/* ── Spawn ────────────────────────────────────────────────────────── */

gboolean
moo_gdb_session_start (MooGdbSession *s, const char *target, GError **error)
{
    g_return_val_if_fail (MOO_IS_GDB_SESSION (s), FALSE);
    if (s->gdb) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
                     "session already started");
        return FALSE;
    }

    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new ((GSubprocessFlags)
            (G_SUBPROCESS_FLAGS_STDIN_PIPE
             | G_SUBPROCESS_FLAGS_STDOUT_PIPE
             | G_SUBPROCESS_FLAGS_STDERR_MERGE));

    /* Build argv: gdb --interpreter=mi3 -nx --quiet [target]
     *   -nx     skip ~/.gdbinit (avoids surprising user-config
     *           interactions; we control the environment).
     *   --quiet suppress the version banner; we'll fetch it with
     *           -gdb-version explicitly. */
    GPtrArray *argv = g_ptr_array_new ();
    g_ptr_array_add (argv, (gpointer) "gdb");
    g_ptr_array_add (argv, (gpointer) "--interpreter=mi3");
    g_ptr_array_add (argv, (gpointer) "-nx");
    g_ptr_array_add (argv, (gpointer) "--quiet");
    if (target && *target)
        g_ptr_array_add (argv, (gpointer) target);
    g_ptr_array_add (argv, NULL);

    s->gdb = g_subprocess_launcher_spawnv (launcher,
                                            (const char *const *) argv->pdata,
                                            error);
    g_ptr_array_free (argv, TRUE);
    g_object_unref (launcher);

    if (!s->gdb) {
        set_state (s, MOO_GDB_STATE_ERROR);
        return FALSE;
    }

    s->gdb_in  = g_subprocess_get_stdin_pipe (s->gdb);
    s->gdb_out = g_data_input_stream_new (
        g_subprocess_get_stdout_pipe (s->gdb));

    set_state (s, MOO_GDB_STATE_LOADING);
    start_read_loop (s);

    /* Fire off the version probe right away.  The reply arrives via
     * the async read loop; on_version_reply stores the result and
     * transitions us to READY. */
    send_command (s, "-gdb-version", on_version_reply, NULL);
    return TRUE;
}

/* ── Quit ─────────────────────────────────────────────────────────── */

void
moo_gdb_session_quit (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!s->gdb) return;
    /* Polite exit; the read loop will see EOF and emit "exited". */
    send_command (s, "-gdb-exit", NULL, NULL);
}

void
moo_gdb_session_shutdown (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));

    /* Cancelling is what releases the reference held by the outstanding
       read (see start_read_loop), so this must be called before the owner
       drops its own reference or the session is never finalized. */
    g_cancellable_cancel (s->cancellable);

    if (s->gdb) {
        g_subprocess_force_exit (s->gdb);
        g_clear_object (&s->gdb);
    }
    s->gdb_in = NULL;
    g_clear_object (&s->gdb_out);
    set_state (s, MOO_GDB_STATE_EXITED);
}

/* ── Raw command forwarding ──────────────────────────────────────── */

void
moo_gdb_session_send_raw (MooGdbSession *s, const char *cmd)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!cmd || !*cmd) return;
    /* Strip a single trailing newline so the user's "info threads\n"
     * doesn't double up.  send_command appends its own. */
    char *trimmed = g_strdup (cmd);
    g_strchomp (trimmed);
    send_command (s, trimmed, NULL, NULL);
    g_free (trimmed);
}

/* ── Sending commands ─────────────────────────────────────────────── */

static void
send_command (MooGdbSession *s, const char *cmd,
              ResponseCB cb, gpointer user_data)
{
    if (!s->gdb_in) return;
    /* gdb_in stays non-NULL after gdb dies (it is owned by the GSubprocess),
       so the state has to be checked as well -- otherwise this writes into a
       closed pipe and raises SIGPIPE. */
    if (s->state == MOO_GDB_STATE_EXITED || s->state == MOO_GDB_STATE_ERROR)
        return;
    if (g_cancellable_is_cancelled (s->cancellable))
        return;
    int token = (int) s->next_token++;
    char *line = g_strdup_printf ("%d%s\n", token, cmd);
    g_output_stream_write_all (s->gdb_in, line, strlen (line),
                                NULL, NULL, NULL);
    g_output_stream_flush (s->gdb_in, NULL, NULL);
    g_free (line);

    if (cb) {
        PendingEntry *pe = g_new (PendingEntry, 1);
        pe->cb        = cb;
        pe->user_data = user_data;
        g_hash_table_insert (s->pending, GINT_TO_POINTER (token), pe);
    }
}

/* ── Async read loop ──────────────────────────────────────────────── */

static void
start_read_loop (MooGdbSession *s)
{
    /* The read is always outstanding while gdb lives, so it can still be
       pending when the last reference to the session goes away.  Cancelling
       does not abort the GTask -- it completes it on the next main-loop turn,
       and the callback would then run on freed memory.  Hold a reference for
       the duration of the read; on_line_async() drops it. */
    g_data_input_stream_read_line_async (
        s->gdb_out, G_PRIORITY_DEFAULT, s->cancellable,
        on_line_async, g_object_ref (s));
}

static void
on_line_async (GObject *source, GAsyncResult *res, gpointer user_data)
{
    MooGdbSession *s = MOO_GDB_SESSION (user_data);
    GError *err = NULL;
    gsize len;
    char *line = g_data_input_stream_read_line_finish (
        G_DATA_INPUT_STREAM (source), res, &len, &err);

    if (!line) {
        /* Cancelled means the session is going away (or already gone) -- do
           not touch it beyond dropping our reference. */
        if (err && g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_error_free (err);
            g_object_unref (s);
            return;
        }
        /* EOF or error → gdb is gone. */
        if (err) g_error_free (err);
        set_state (s, MOO_GDB_STATE_EXITED);
        g_signal_emit (s, signals[SIG_EXITED], 0);
        g_object_unref (s);
        return;
    }

    MooGdbMiRecord *rec = moo_gdb_mi_parse (line);
    g_free (line);
    if (rec) {
        dispatch_record (s, rec);
        moo_gdb_mi_record_free (rec);
    }

    /* Keep reading.  start_read_loop() takes its own reference, so ours is
       released afterwards. */
    start_read_loop (s);
    g_object_unref (s);
}

static void
dispatch_record (MooGdbSession *s, MooGdbMiRecord *r)
{
    MooGdbMiKind k = moo_gdb_mi_record_kind (r);
    switch (k) {
    case MOO_GDB_MI_RESULT: {
        const char *klass = moo_gdb_mi_record_class (r);
        int tok = moo_gdb_mi_record_token (r);
        PendingEntry *pe = NULL;
        if (tok >= 0)
            pe = (PendingEntry *) g_hash_table_lookup (
                s->pending, GINT_TO_POINTER (tok));

        /* Errors with a matched callback go to that callback (which
         * gets the full reply record and can read `msg` itself).
         * Only orphan errors — those for tokens nobody is waiting on,
         * plus token-matched ones whose registered callback is NULL
         * — fire the global SIG_ERROR.  This stops hover-eval's
         * expected "No symbol X in current context" / "No registers."
         * replies from spamming the console while the user mouses
         * over comments or after the inferior has exited. */
        if (klass && !strcmp (klass, "error") &&
            (!pe || pe->cb == NULL))
        {
            MooGdbMiValue *msg = moo_gdb_mi_record_field (r, "msg");
            const char *mstr = msg ? moo_gdb_mi_value_string (msg) : NULL;
            g_signal_emit (s, signals[SIG_ERROR], 0,
                           mstr ? mstr : "(unknown gdb error)");
        }
        if (tok >= 0) {
            if (pe) {
                ResponseCB cb = pe->cb;
                gpointer ud   = pe->user_data;
                /* Remove before invoking so the callback can safely
                 * issue another command. */
                g_hash_table_remove (s->pending, GINT_TO_POINTER (tok));
                if (cb) cb (s, r, ud);
            }
        }
        break;
    }
    case MOO_GDB_MI_CONSOLE: {
        const char *t = moo_gdb_mi_record_text (r);
        if (t) g_signal_emit (s, signals[SIG_CONSOLE_OUTPUT], 0, t);
        break;
    }
    case MOO_GDB_MI_LOG: {
        const char *t = moo_gdb_mi_record_text (r);
        if (t) g_signal_emit (s, signals[SIG_LOG_OUTPUT], 0, t);
        break;
    }
    case MOO_GDB_MI_TARGET: {
        const char *t = moo_gdb_mi_record_text (r);
        if (t) g_signal_emit (s, signals[SIG_TARGET_OUTPUT], 0, t);
        break;
    }
    case MOO_GDB_MI_PROMPT:
        if (s->state == MOO_GDB_STATE_LOADING)
            set_state (s, MOO_GDB_STATE_READY);
        break;
    case MOO_GDB_MI_EXEC: {
        const char *klass = moo_gdb_mi_record_class (r);
        if (!klass) break;
        if (!strcmp (klass, "running")) {
            set_state (s, MOO_GDB_STATE_RUNNING);
            /* Varobjs are frame-bound; drop them all before the next
             * stop so we don't accidentally reuse a stale one whose
             * underlying address has been freed/changed. */
            for (guint i = 0; i < s->varobjs->len; i++) {
                char *name = (char *) s->varobjs->pdata[i];
                char *cmd  = g_strdup_printf ("-var-delete %s", name);
                send_command (s, cmd, NULL, NULL);
                g_free (cmd);
            }
            g_ptr_array_set_size (s->varobjs, 0);
            s->inferior_alive = TRUE;
            g_signal_emit (s, signals[SIG_RUNNING], 0);
        } else if (!strcmp (klass, "stopped")) {
            /* Pull file/line/function from the optional frame={…} */
            const char *file = NULL, *func = NULL;
            int         line = 0;
            MooGdbMiValue *frame = moo_gdb_mi_record_field (r, "frame");
            if (frame) {
                MooGdbMiValue *v;
                /* fullname is gdb's canonical absolute path; fall
                 * back to relative `file` if missing. */
                v = moo_gdb_mi_value_tuple_get (frame, "fullname");
                if (!v) v = moo_gdb_mi_value_tuple_get (frame, "file");
                if (v) file = moo_gdb_mi_value_string (v);
                v = moo_gdb_mi_value_tuple_get (frame, "line");
                if (v) {
                    const char *ls = moo_gdb_mi_value_string (v);
                    if (ls) line = atoi (ls);
                }
                v = moo_gdb_mi_value_tuple_get (frame, "func");
                if (v) func = moo_gdb_mi_value_string (v);
            }
            const char *reason = NULL;
            MooGdbMiValue *rv = moo_gdb_mi_record_field (r, "reason");
            if (rv) reason = moo_gdb_mi_value_string (rv);

            set_state (s, MOO_GDB_STATE_STOPPED);
            /* gdb reports inferior termination via *stopped,reason=
             * exited (or "exited-normally" / "exited-signalled").
             * The session state stays STOPPED — gdb is still alive
             * and the user can re-launch — but there's no longer a
             * frame to inspect, so suppress hover/locals/watch evals
             * that would otherwise come back as "No registers." */
            if (reason && g_str_has_prefix (reason, "exited")) {
                s->inferior_alive = FALSE;
            }
            g_signal_emit (s, signals[SIG_STOPPED], 0,
                           reason, file, line, func);
            /* Auto-refresh the locals snapshot so the panel can
             * update without each consumer needing to wire up its
             * own -stack-list-variables.  Cheap when there are no
             * locals (e.g. stopped in glibc with no debug info).
             * Skip the request burst when the inferior is gone —
             * gdb would just return "No registers." for every one. */
            if (s->inferior_alive) {
                request_locals_refresh (s);
                request_frames_refresh (s);
                request_watches_refresh (s);
            }
        }
        break;
    }
    case MOO_GDB_MI_NOTIFY: {
        const char *klass = moo_gdb_mi_record_class (r);
        if (!klass) break;
        if (!strcmp (klass, "breakpoint-created")
            || !strcmp (klass, "breakpoint-modified"))
        {
            MooGdbMiValue *bkpt = moo_gdb_mi_record_field (r, "bkpt");
            emit_bp_added_from_bkpt (s, bkpt);
        }
        else if (!strcmp (klass, "breakpoint-deleted"))
        {
            MooGdbMiValue *idv = moo_gdb_mi_record_field (r, "id");
            if (idv) {
                const char *idstr = moo_gdb_mi_value_string (idv);
                if (idstr)
                    g_signal_emit (s, signals[SIG_BP_REMOVED],
                                   0, atoi (idstr));
            }
        }
        break;
    }
    default:
        /* STATUS / TARGET — wired in later commits. */
        break;
    }
}

/* ── Target / arguments / cwd ────────────────────────────────────── */

void
moo_gdb_session_set_target (MooGdbSession *s, const char *target)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!s->gdb_in) return;
    char *escaped = target
        ? g_strescape (target, "")
        : g_strdup ("");
    char *cmd = g_strdup_printf ("-file-exec-and-symbols \"%s\"", escaped);
    send_command (s, cmd, NULL, NULL);
    g_free (cmd);
    g_free (escaped);
}

void
moo_gdb_session_set_args (MooGdbSession *s, const char *const *argv)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!s->gdb_in) return;
    /* `-exec-arguments arg1 arg2 ...` — each arg shell-quoted to
     * survive embedded spaces.  Empty argv clears the list. */
    GString *cmd = g_string_new ("-exec-arguments");
    if (argv) {
        for (int i = 0; argv[i]; i++) {
            char *quoted = g_shell_quote (argv[i]);
            g_string_append_c (cmd, ' ');
            g_string_append   (cmd, quoted);
            g_free (quoted);
        }
    }
    send_command (s, cmd->str, NULL, NULL);
    g_string_free (cmd, TRUE);
}

void
moo_gdb_session_set_cwd (MooGdbSession *s, const char *cwd)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    if (!s->gdb_in || !cwd) return;
    /* GDB MI argument quoting is C-string style: double quotes with
     * backslash-escapes.  Shell-style single-quote wrapping
     * (`'/path/...'`) is NOT recognised — gdb treats the surrounding
     * apostrophes as part of the filename and reports `No such file
     * or directory.` for a path that actually exists.  Mirror what
     * moo_gdb_session_set_target does for `-file-exec-and-symbols`. */
    char *escaped = g_strescape (cwd, "");
    char *cmd     = g_strdup_printf ("-environment-cd \"%s\"", escaped);
    send_command (s, cmd, NULL, NULL);
    g_free (cmd);
    g_free (escaped);
}

/* ── Execution control ────────────────────────────────────────────── */

void
moo_gdb_session_run (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    /* -exec-run starts the inferior from main(); the *running record
     * arrives first, then *stopped on the first breakpoint / signal. */
    send_command (s, "-exec-run", NULL, NULL);
}

void
moo_gdb_session_continue (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    send_command (s, "-exec-continue", NULL, NULL);
}

/* -exec-next advances over a single source line; if the line is a
 * function call, the call returns before gdb stops again. */
void
moo_gdb_session_step_over (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    send_command (s, "-exec-next", NULL, NULL);
}

/* -exec-step advances by one source line, descending into any
 * function call on that line. */
void
moo_gdb_session_step_into (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    send_command (s, "-exec-step", NULL, NULL);
}

/* -exec-finish runs until the current function returns. */
void
moo_gdb_session_step_out (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    send_command (s, "-exec-finish", NULL, NULL);
}

/* -exec-interrupt sends SIGINT to the running inferior.  Use this
 * to break into a hung program.  gdb responds with the usual
 * *stopped record (reason="signal-received") which our existing
 * dispatcher already routes to set_exec_mark. */
void
moo_gdb_session_pause (MooGdbSession *s)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    /* --all interrupts every thread in a multi-threaded program. */
    send_command (s, "-exec-interrupt --all", NULL, NULL);
}

/* ── Breakpoints ──────────────────────────────────────────────────── */

/* Decode a `bkpt={number="N",fullname="...",file="...",line="L",...}`
 * value into (number, file, line) and emit "breakpoint-added".
 * `bkpt_val` may be NULL — we just return without firing.
 *
 * Pending breakpoints (set via -break-insert -f before any binary
 * has been loaded) come back without fullname/file/line; gdb stuffs
 * the original location into `original-location="file:line"`
 * instead.  Parse that fallback so the UI's placeholder
 * (registered under the same file:line key it requested) can still
 * find a match. */
static void
emit_bp_added_from_bkpt (MooGdbSession *s, MooGdbMiValue *bkpt_val)
{
    if (!bkpt_val) return;
    MooGdbMiValue *v;
    int   number = -1;
    const char *file = NULL;
    int   line = 0;
    /* Buffer for the parsed-from-original-location filename — must
     * outlive the signal emission below. */
    char  file_buf[1024];
    file_buf[0] = '\0';

    v = moo_gdb_mi_value_tuple_get (bkpt_val, "number");
    if (v) {
        const char *ns = moo_gdb_mi_value_string (v);
        if (ns) number = atoi (ns);
    }
    v = moo_gdb_mi_value_tuple_get (bkpt_val, "fullname");
    if (!v) v = moo_gdb_mi_value_tuple_get (bkpt_val, "file");
    if (v) file = moo_gdb_mi_value_string (v);
    v = moo_gdb_mi_value_tuple_get (bkpt_val, "line");
    if (v) {
        const char *ls = moo_gdb_mi_value_string (v);
        if (ls) line = atoi (ls);
    }

    /* Fallback for pending breakpoints. */
    if (!file) {
        v = moo_gdb_mi_value_tuple_get (bkpt_val, "original-location");
        if (v) {
            const char *orig = moo_gdb_mi_value_string (v);
            if (orig) {
                const char *colon = strrchr (orig, ':');
                if (colon && colon > orig
                    && (gsize)(colon - orig) < sizeof file_buf)
                {
                    memcpy (file_buf, orig, colon - orig);
                    file_buf[colon - orig] = '\0';
                    file = file_buf;
                    if (line <= 0)
                        line = atoi (colon + 1);
                }
            }
        }
    }

    /* Still no file — gdb gave us a function-name breakpoint or
     * similar location we can't easily map to a margin mark.  Drop
     * silently rather than crash downstream consumers that key by
     * (file, line). */
    if (number < 0 || !file) return;

    g_signal_emit (s, signals[SIG_BP_ADDED], 0, number, file, line);
}

static void
on_break_insert_reply (MooGdbSession *s, MooGdbMiRecord *r,
                       G_GNUC_UNUSED gpointer user_data)
{
    const char *klass = moo_gdb_mi_record_class (r);
    if (!klass || strcmp (klass, "done") != 0) return;
    MooGdbMiValue *bkpt = moo_gdb_mi_record_field (r, "bkpt");
    emit_bp_added_from_bkpt (s, bkpt);
}

void
moo_gdb_session_break_add (MooGdbSession *s, const char *file, int line)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    g_return_if_fail (file != NULL && line > 0);
    /* -break-insert -f FILE:LINE — the -f flag makes the breakpoint
     * "pending": gdb accepts it even if no symbol table is loaded
     * yet and resolves it later when the inferior gets loaded.
     *
     * NB: do NOT shell-quote the path here — gdb echoes the
     * original location back via `original-location="..."`, and
     * adding `'` quotes makes the round-trip path mismatch the
     * placeholder we registered under the unquoted path.  Paths
     * with embedded spaces would be a problem; if/when that hits,
     * use MI-quoting (`"..."`) rather than shell-quoting. */
    char *cmd = g_strdup_printf ("-break-insert -f %s:%d", file, line);
    send_command (s, cmd, on_break_insert_reply, NULL);
    g_free (cmd);
}

void
moo_gdb_session_break_remove (MooGdbSession *s, int number)
{
    g_return_if_fail (MOO_IS_GDB_SESSION (s));
    g_return_if_fail (number >= 0);
    char *cmd = g_strdup_printf ("-break-delete %d", number);
    send_command (s, cmd, NULL, NULL);
    g_free (cmd);
    /* gdb sends =breakpoint-deleted async on success; that's the
     * primary signal path — we emit "breakpoint-removed" from the
     * NOTIFY branch of the dispatcher.  Emit it locally too in case
     * the deletion races: harmless if duplicated. */
    g_signal_emit (s, signals[SIG_BP_REMOVED], 0, number);
}

/* ── -gdb-version reply ───────────────────────────────────────────── */

static void
on_version_reply (MooGdbSession *s, MooGdbMiRecord *r,
                  G_GNUC_UNUSED gpointer user_data)
{
    /* `-gdb-version` doesn't return its data structured; instead it
     * streams the version through `~"..."` CONSOLE records, then a
     * single `^done` arrives.  Our console handler already emits the
     * lines; here we just stash the most-recent banner-looking line. */
    const char *klass = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done") && !s->version)
        s->version = g_strdup ("(see console output)");
}

/* ── Locals refresh ──────────────────────────────────────────────── */

static void
request_locals_refresh (MooGdbSession *s)
{
    /* --simple-values asks gdb to fill in `value` for scalar types
     * and omit it for aggregates (struct / array / union).  Phase
     * 4a doesn't drill into aggregates; commit 9b can add
     * -var-create for that. */
    send_command (s,
                  "-stack-list-variables --simple-values",
                  on_locals_reply, NULL);
}

static void
on_locals_reply (MooGdbSession *s, MooGdbMiRecord *r,
                 G_GNUC_UNUSED gpointer user_data)
{
    /* Expected shape:
     *   ^done,variables=[{name="x",type="int",value="10"},
     *                    {name="argv",type="char **",value="0x..."},
     *                    {name="argc",type="int",value="1"}, ...]
     * with `value` and/or `type` optionally missing per cell. */
    g_ptr_array_set_size (s->locals, 0);

    const char *klass = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done")) {
        MooGdbMiValue *vars = moo_gdb_mi_record_field (r, "variables");
        if (vars) {
            guint n = moo_gdb_mi_value_list_len (vars);
            for (guint i = 0; i < n; i++) {
                MooGdbMiValue *cell = moo_gdb_mi_value_list_nth (vars, i);
                if (!cell) continue;
                MooGdbLocal *l = g_new0 (MooGdbLocal, 1);
                MooGdbMiValue *v;
                v = moo_gdb_mi_value_tuple_get (cell, "name");
                if (v) l->name  = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (cell, "type");
                if (v) l->type  = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (cell, "value");
                if (v) l->value = g_strdup (moo_gdb_mi_value_string (v));
                if (!l->name) {
                    free_local (l);
                    continue;
                }
                g_ptr_array_add (s->locals, l);
            }
        }
    }

    g_signal_emit (s, signals[SIG_LOCALS_CHANGED], 0);
}

/* ── Frames refresh ──────────────────────────────────────────────── */

static void
request_frames_refresh (MooGdbSession *s)
{
    send_command (s, "-stack-list-frames", on_frames_reply, NULL);
}

static void
on_frames_reply (MooGdbSession *s, MooGdbMiRecord *r,
                 G_GNUC_UNUSED gpointer user_data)
{
    /* Expected shape:
     *   ^done,stack=[frame={level="0",addr="0x...",func="main",
     *                       file="t.c",fullname="/tmp/t.c",line="6"},
     *                frame={level="1",addr="0x...",func="__libc_start_main",
     *                       file="...",line="..."},
     *                ...] */
    g_ptr_array_set_size (s->frames, 0);

    const char *klass = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done")) {
        MooGdbMiValue *stack = moo_gdb_mi_record_field (r, "stack");
        if (stack) {
            guint n = moo_gdb_mi_value_list_len (stack);
            for (guint i = 0; i < n; i++) {
                MooGdbMiValue *cell = moo_gdb_mi_value_list_nth (stack, i);
                if (!cell) continue;
                MooGdbFrame *f = g_new0 (MooGdbFrame, 1);
                MooGdbMiValue *v;
                v = moo_gdb_mi_value_tuple_get (cell, "level");
                if (v) {
                    const char *ls = moo_gdb_mi_value_string (v);
                    if (ls) f->level = atoi (ls);
                }
                v = moo_gdb_mi_value_tuple_get (cell, "func");
                if (v) f->function = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (cell, "fullname");
                if (!v) v = moo_gdb_mi_value_tuple_get (cell, "file");
                if (v) f->file = g_strdup (moo_gdb_mi_value_string (v));
                v = moo_gdb_mi_value_tuple_get (cell, "line");
                if (v) {
                    const char *ls = moo_gdb_mi_value_string (v);
                    if (ls) f->line = atoi (ls);
                }
                v = moo_gdb_mi_value_tuple_get (cell, "addr");
                if (v) f->addr = g_strdup (moo_gdb_mi_value_string (v));
                g_ptr_array_add (s->frames, f);
            }
        }
    }

    g_signal_emit (s, signals[SIG_FRAMES_CHANGED], 0);
}

/* ── Watch refresh ───────────────────────────────────────────────── */

static void
request_watches_refresh (MooGdbSession *s)
{
    /* Fire off one -data-evaluate-expression per occupied slot;
     * each reply lands in on_watch_reply which writes the value
     * back to the matching slot and emits "watches-changed" after
     * the last pending eval finishes. */
    for (guint slot = 0; slot < s->watches->len; slot++) {
        MooGdbWatch *w = (MooGdbWatch *) s->watches->pdata[slot];
        if (!w || !w->expression) continue;
        moo_gdb_session_eval_watch (s, slot, w->expression);
    }
}

static void
on_watch_reply (MooGdbSession *s, MooGdbMiRecord *r, gpointer user_data)
{
    guint slot = GPOINTER_TO_UINT (user_data) - 1;
    if (slot >= s->watches->len) return;
    MooGdbWatch *w = (MooGdbWatch *) s->watches->pdata[slot];
    if (!w) return;

    g_free (w->value);
    w->value = NULL;
    w->error = FALSE;

    const char *klass = moo_gdb_mi_record_class (r);
    if (klass && !strcmp (klass, "done")) {
        MooGdbMiValue *v = moo_gdb_mi_record_field (r, "value");
        if (v) w->value = g_strdup (moo_gdb_mi_value_string (v));
    } else if (klass && !strcmp (klass, "error")) {
        MooGdbMiValue *m = moo_gdb_mi_record_field (r, "msg");
        if (m) w->value = g_strdup (moo_gdb_mi_value_string (m));
        w->error = TRUE;
    }

    /* Fire once per reply.  In practice the UI doesn't care which
     * specific watch changed — it just rebuilds the whole list. */
    g_signal_emit (s, signals[SIG_WATCHES_CHANGED], 0);
}
