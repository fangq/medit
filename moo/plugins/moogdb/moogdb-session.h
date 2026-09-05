/*
 *   moogdb-session.h
 *
 *   Public API for the MooGdbSession GObject — a thin wrapper around
 *   a `gdb --interpreter=mi3` subprocess.  Phase 1 (commit 1) only
 *   exposes the spawn-and-quit lifecycle plus a `-gdb-version`
 *   round-trip; execution control, breakpoints, and frame inspection
 *   land in subsequent commits.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#ifndef MOO_GDB_SESSION_H
#define MOO_GDB_SESSION_H

#include <glib-object.h>

G_BEGIN_DECLS

/* High-level session state.  Driven by the MI exec/notify stream
 * dispatcher; UI code subscribes to "state-changed" to know when to
 * enable/disable the toolbar buttons. */
typedef enum {
    MOO_GDB_STATE_IDLE,       /* created but gdb not yet spawned     */
    MOO_GDB_STATE_LOADING,    /* gdb spawned, waiting for prompt     */
    MOO_GDB_STATE_READY,      /* prompt seen, ready to accept cmds   */
    MOO_GDB_STATE_RUNNING,    /* target executing (Phase 2)          */
    MOO_GDB_STATE_STOPPED,    /* target stopped (Phase 2)            */
    MOO_GDB_STATE_EXITED,     /* gdb has quit                        */
    MOO_GDB_STATE_ERROR
} MooGdbState;

#define MOO_TYPE_GDB_SESSION (moo_gdb_session_get_type ())
G_DECLARE_FINAL_TYPE (MooGdbSession, moo_gdb_session,
                      MOO, GDB_SESSION, GObject)

/* Lifecycle ----------------------------------------------------------- */

/* Create a session.  Does NOT spawn gdb yet — call moo_gdb_session_start. */
MooGdbSession *moo_gdb_session_new       (void);

/* Spawn `gdb --interpreter=mi3 -nx --quiet [target]`.  `target` may
 * be NULL for a session that loads its target later.  Returns FALSE
 * and sets `error` if the spawn failed (e.g. gdb not on PATH). */
gboolean       moo_gdb_session_start     (MooGdbSession *session,
                                          const char    *target,
                                          GError       **error);

/* Send "-gdb-exit".  Fire-and-forget: the subprocess dies asynchronously and
 * the read loop reports it via the "exited" signal.  Safe to call even if the
 * session was never started. */
void           moo_gdb_session_quit      (MooGdbSession *session);

/* Hard teardown: cancel the read loop, force gdb down and mark the session
 * EXITED.  The read loop holds a reference to the session, so an owner MUST
 * call this before dropping its own reference -- otherwise the session (and
 * the gdb process) leak for the lifetime of the application. */
void           moo_gdb_session_shutdown  (MooGdbSession *session);

/* Forward a raw command line to gdb's stdin.  Useful for the
 * console pane: any CLI command (e.g. "info threads", "print x",
 * or any -MI- command) is sent verbatim, prefixed only with the
 * usual numeric token so the reply can be matched.  Empty / NULL
 * lines are no-ops. */
void           moo_gdb_session_send_raw  (MooGdbSession *s,
                                          const char    *cmd);

/* One local variable in the current stack frame.  Returned by
 * moo_gdb_session_get_locals as borrowed pointers — the session
 * owns the storage and recycles it on the next *stopped. */
typedef struct {
    char *name;   /* always non-NULL */
    char *type;   /* may be NULL for --simple-values output */
    char *value;  /* may be NULL for aggregates we didn't drill into */
} MooGdbLocal;

/* Snapshot of the variables in the current frame after the most
 * recent *stopped event.  Transfer-none.  Empty if the session
 * hasn't stopped yet (or the inferior has no locals at this PC). */
GPtrArray     *moo_gdb_session_get_locals (MooGdbSession *s);

/* One stack frame in the current thread's backtrace. */
typedef struct {
    int   level;     /* 0 = innermost (currently-executing) */
    char *function;  /* function name (may be NULL) */
    char *file;      /* canonical source path (may be NULL) */
    int   line;      /* 1-based, 0 if not available */
    char *addr;      /* hex address (may be NULL) */
} MooGdbFrame;

/* Snapshot of the call stack after the most recent *stopped.
 * Transfer-none; entries owned by the session. */
GPtrArray     *moo_gdb_session_get_frames (MooGdbSession *s);

/* Switch the selected stack frame.  Triggers a -stack-select-frame
 * and re-requests locals at that frame, so the Locals pane refreshes
 * automatically.  `level` 0 = innermost frame; deeper frames go up. */
void           moo_gdb_session_select_frame (MooGdbSession *s, int level);

/* Evaluate `expr` in the current frame and store the result on the
 * given watch slot.  Called by the watch UI after the user adds a
 * new expression, and automatically on each *stopped event for
 * every registered watch.  The result lands in the watches array;
 * "watches-changed" fires when all pending evaluations finish. */
void           moo_gdb_session_eval_watch  (MooGdbSession *s,
                                            guint          slot,
                                            const char    *expr);

/* One watch slot — the expression the user typed and its most
 * recent value (or error message). */
typedef struct {
    char    *expression;   /* the user's text */
    char    *value;        /* most-recent eval result; NULL if unset */
    gboolean error;        /* TRUE if the last eval returned ^error */
} MooGdbWatch;

/* Borrowed snapshot of all registered watches.  Each element is a
 * MooGdbWatch* indexed by slot number.  Slots may be NULL if a
 * watch was removed. */
GPtrArray     *moo_gdb_session_get_watches (MooGdbSession *s);

/* Allocate a fresh slot for a new watch and return its index.
 * The expression is stored but not evaluated until a stop. */
guint          moo_gdb_session_add_watch   (MooGdbSession *s,
                                            const char    *expr);
void           moo_gdb_session_remove_watch(MooGdbSession *s, guint slot);

/* ── GDB variable objects (for drill-into-aggregates) ────────────────
 *
 * A "varobj" is gdb's handle for an expression that lives across MI
 * commands.  We use it to lazily expand structs / arrays in the Locals
 * pane: when the user expands a row, we ask gdb to give us the
 * children of that variable.  The session tracks created varobjs and
 * deletes them on every *running so the next stop starts with a fresh
 * frame.  Callers shouldn't worry about lifecycle. */

typedef struct {
    char *name;     /* gdb-assigned varobj name (e.g. "var1.field") */
    char *exp;      /* short display text — the field/index name    */
    char *type;
    char *value;    /* same caveat as MooGdbLocal::value             */
    int   numchild; /* 0 means not further expandable                */
} MooGdbVarChild;

typedef void (*MooGdbVarCreateCb) (MooGdbSession *s,
                                   const char    *expr,
                                   const char    *varobj_name,
                                   const char    *type,
                                   const char    *value,
                                   int            numchild,
                                   gboolean       is_error,
                                   gpointer       user_data);

/* Children list ownership: borrowed; callback must g_ptr_array_ref
 * if it wants to keep it past the callback return. */
typedef void (*MooGdbVarChildrenCb) (MooGdbSession *s,
                                     const char    *parent_varobj,
                                     GPtrArray     *children,
                                     gboolean       is_error,
                                     gpointer       user_data);

void  moo_gdb_session_var_create   (MooGdbSession *s, const char *expr,
                                    MooGdbVarCreateCb cb, gpointer user_data);
void  moo_gdb_session_var_children (MooGdbSession *s, const char *varobj_name,
                                    MooGdbVarChildrenCb cb,
                                    gpointer user_data);

/* One-shot expression evaluator — fire `-data-evaluate-expression`
 * for `expr` in the current frame and deliver the result via `cb`.
 * The `value` argument to the callback is gdb's `value` field on
 * success, or its `msg` field on failure (with `is_error = TRUE`).
 * If the session is not stopped, the callback is invoked synchronously
 * with `value = NULL, is_error = TRUE` so callers don't need a separate
 * "not ready" code path.  Used by the source-view hover tooltip. */
typedef void (*MooGdbEvalCb) (MooGdbSession *s,
                              const char    *expr,
                              const char    *value,
                              gboolean       is_error,
                              gpointer       user_data);
void           moo_gdb_session_eval_async  (MooGdbSession *s,
                                            const char    *expr,
                                            MooGdbEvalCb   cb,
                                            gpointer       user_data);

/* Set the target executable to debug.  May be called before or
 * after start(); the path is forwarded to gdb via `-file-exec-and-
 * symbols`.  Safe to set NULL (clears the current target). */
void           moo_gdb_session_set_target (MooGdbSession *s,
                                           const char    *target);

/* Set the program arguments (NULL-terminated argv-style; the
 * binary itself is NOT in the list).  Sent to gdb via
 * `-exec-arguments` so a later `-exec-run` picks them up. */
void           moo_gdb_session_set_args   (MooGdbSession     *s,
                                           const char *const *argv);

/* Set the working directory for the target program.  Forwarded
 * via `-environment-cd`. */
void           moo_gdb_session_set_cwd    (MooGdbSession *s,
                                           const char    *cwd);

/* Execution control.  All async; subscribe to "state-changed"
 * (and Phase-4's "stopped" / "running") to observe transitions. */
void           moo_gdb_session_run        (MooGdbSession *s);
void           moo_gdb_session_continue   (MooGdbSession *s);
void           moo_gdb_session_step_over  (MooGdbSession *s);   /* -exec-next   */
void           moo_gdb_session_step_into  (MooGdbSession *s);   /* -exec-step   */
void           moo_gdb_session_step_out   (MooGdbSession *s);   /* -exec-finish */
/* Send SIGINT to the inferior so the user can break into a hung
 * program.  After this fires the user gets the usual *stopped
 * notification with reason="signal-received". */
void           moo_gdb_session_pause      (MooGdbSession *s);   /* -exec-interrupt */

/* Breakpoints.  Returns the gdb-assigned number async via the
 * "breakpoint-added" signal — there's no synchronous return value
 * because the -break-insert reply may not arrive immediately. */
void           moo_gdb_session_break_add  (MooGdbSession *s,
                                           const char    *file,
                                           int            line);
void           moo_gdb_session_break_remove(MooGdbSession *s, int number);

/* State accessors ---------------------------------------------------- */

MooGdbState    moo_gdb_session_get_state (MooGdbSession *session);

/* TRUE while an inferior process is loaded and has a live frame to
 * inspect — set after *running and cleared when *stopped arrives
 * with a reason starting with "exited" (the program completed).
 * The high-level state stays MOO_GDB_STATE_STOPPED in that case
 * because gdb itself is still alive and ready to run again; this
 * finer-grained flag is what hover-to-inspect and the locals/watch
 * refresh paths check so they don't issue evaluations that gdb
 * would just reject with "No registers." */
gboolean       moo_gdb_session_inferior_alive (MooGdbSession *session);

/* Most-recent `-gdb-version` reply, or NULL until populated. */
const char    *moo_gdb_session_get_version (MooGdbSession *session);

/* Signals (on the session GObject) ------------------------------------
 *
 *   "state-changed"  :: ()
 *       fired whenever moo_gdb_session_get_state() would return
 *       a different value than the last firing.
 *
 *   "console-output" :: (const char *line)
 *       a single CLI-style line from gdb (~"..." MI record).
 *       Already de-escaped.
 *
 *   "log-output"     :: (const char *line)
 *       gdb's own internal log lines (&"..." MI record).
 *
 *   "target-output"  :: (const char *line)
 *       inferior stdout/stderr captured by gdb (@"..." MI record).
 *       Where the user's printf() output lands.
 *
 *   "running"             :: ()
 *       fired on the *running async record.
 *
 *   "stopped"             :: (reason, file, line, function)
 *       carries the file/line/function gdb reported in the
 *       *stopped record's frame={...} field.  All strings borrowed.
 *
 *   "breakpoint-added"    :: (int number, const char *file, int line)
 *       -break-insert succeeded.
 *
 *   "breakpoint-removed"  :: (int number)
 *       breakpoint identified by `number` was removed.
 *
 *   "locals-changed"      :: ()
 *       moo_gdb_session_get_locals now returns a fresh snapshot.
 *       Fires after every *stopped event once the auto-requested
 *       -stack-list-variables reply has been parsed.
 *
 *   "frames-changed"      :: ()
 *       moo_gdb_session_get_frames now returns a fresh snapshot.
 *       Fires after every *stopped event once -stack-list-frames
 *       has been parsed.
 *
 *   "watches-changed"     :: ()
 *       moo_gdb_session_get_watches now returns a fresh snapshot.
 *       Fires after each batch of -data-evaluate-expression
 *       replies (one per registered watch) lands.
 *
 *   "exited"              :: ()
 *       gdb has terminated.  The session is no longer usable.
 */

G_END_DECLS

#endif /* MOO_GDB_SESSION_H */
