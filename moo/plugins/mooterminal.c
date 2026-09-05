/*
 *   mooterminal.c
 *
 *   Native VTE terminal plugin for medit — replaces terminal.py
 *   No Python dependency required.
 *
 *   Supports Terminator-style recursive horizontal/vertical splitting.
 *
 *   Copyright (C) 2004-2010 by Yevgen Muntyan <emuntyan@users.sourceforge.net>
 *   Copyright (C) 2024-2025 — C port
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include <vte/vte.h>
#include <string.h>
#include <stdlib.h>
#ifndef G_OS_WIN32
#include <pwd.h>
#include <unistd.h>
#endif

#include "mooedit/mooplugin.h"
#include "mooedit/mooplugin-macro.h"
#include "mooedit/mooeditwindow.h"
#include "mooedit/mooedit.h"
#include "mooutils/moopane.h"
#include "mooutils/moostock.h"
#include "mooutils/mooprefs.h"
#include "mooutils/mooi18n.h"

/* ================================================================ */
/* Preferences keys                                                 */
/* ================================================================ */

#define TERMINAL_PLUGIN_ID      "Terminal"
#define COLOR_SCHEME_KEY        "Plugins/Terminal/color_scheme"
#define SHELL_KEY               "Plugins/Terminal/shell"
#define FONT_KEY                "Plugins/Terminal/font"
#define SCROLLBACK_KEY          "Plugins/Terminal/scrollback_lines"

/* ================================================================ */
/* Forward declarations                                             */
/* ================================================================ */

static GtkWidget *create_terminal_box (void);

static void on_cd_activate       (GtkMenuItem *item, VteTerminal *term);
static void on_pushd_activate    (GtkMenuItem *item, VteTerminal *term);
static void on_copy_activate     (GtkMenuItem *item, VteTerminal *term);
static void on_paste_activate    (GtkMenuItem *item, VteTerminal *term);
static void on_font_activate     (GtkMenuItem *item, VteTerminal *term);
static void on_scheme_activate   (GtkMenuItem *item, VteTerminal *term);
static void on_split_h_activate  (GtkMenuItem *item, VteTerminal *term);
static void on_split_v_activate  (GtkMenuItem *item, VteTerminal *term);
static void on_close_activate    (GtkMenuItem *item, VteTerminal *term);

static gboolean on_button_press  (VteTerminal *term, GdkEventButton *event,
                                   gpointer data);
static gboolean on_scroll_event  (GtkWidget *widget, GdkEventScroll *event,
                                   gpointer data);
static char    *get_user_shell   (void);
static gboolean shell_supports_pushd (const char *shell);

/* ================================================================ */
/* Color schemes — ported from terminal.py (stolen from Konsole)    */
/* ================================================================ */

typedef struct {
    const char *name;
    const char *fg;
    const char *bg;
    const char *palette[16];  /* 16 ANSI colors */
    gboolean    has_colors;
} TermColorScheme;

static const TermColorScheme color_schemes[] = {
    { "Default",
      "#ececec", "#000000",
      { "#171421", "#c01c28", "#18b218", "#a2734c",
        "#12488b", "#b218b2", "#2aa1b3", "#d0cfcc",
        "#5e5c64", "#f66151", "#33d17a", "#e9ad0c",
        "#2a7bde", "#c061cb", "#33c7de", "#ffffff" },
      TRUE },
    { "Black on White",
      "#000000", "#ffffff",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "Black on Light Yellow",
      "#000000", "#ffffdd",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "Marble",
      "#ffffff", "#000000",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "Green on Black",
      "#18f018", "#000000",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "Paper, Light",
      "#000000", "#ffffff",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "Paper",
      "#000000", "#ffffff",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "Linux Colors",
      "#b2b2b2", "#000000",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
    { "VIM Colors",
      "#000000", "#ffffff",
      { "#000000", "#c00000", "#008000", "#808000",
        "#0000c0", "#c000c0", "#008080", "#c0c0c0",
        "#808080", "#ff6060", "#00ff00", "#ffff00",
        "#8080ff", "#ff40ff", "#00ffff", "#ffffff" },
      TRUE },
    { "White on Black",
      "#ffffff", "#000000",
      { "#000000", "#b21818", "#18b218", "#b26818",
        "#1818b2", "#b218b2", "#18b2b2", "#b2b2b2",
        "#686868", "#ff5454", "#54ff54", "#ffff54",
        "#5454ff", "#ff54ff", "#54ffff", "#ffffff" },
      TRUE },
};

#define N_COLOR_SCHEMES (sizeof(color_schemes) / sizeof(color_schemes[0]))

static const TermColorScheme *
find_color_scheme (const char *name)
{
    guint i;
    if (!name || !name[0])
        return &color_schemes[0];
    for (i = 0; i < N_COLOR_SCHEMES; i++) {
        if (g_strcmp0 (color_schemes[i].name, name) == 0)
            return &color_schemes[i];
    }
    return &color_schemes[0];
}

static void
apply_color_scheme (VteTerminal *term, const TermColorScheme *cs)
{
    if (!cs || !cs->has_colors)
        return;

    GdkRGBA fg, bg;
    GdkRGBA palette[16];
    int i;

    gdk_rgba_parse (&fg, cs->fg);
    gdk_rgba_parse (&bg, cs->bg);

    for (i = 0; i < 16; i++)
        gdk_rgba_parse (&palette[i], cs->palette[i]);

    vte_terminal_set_color_foreground (term, &fg);
    vte_terminal_set_color_background (term, &bg);
    vte_terminal_set_colors (term, &fg, &bg, palette, 16);
}

/* ================================================================ */
/* Shell detection                                                  */
/* ================================================================ */

static char *
get_user_shell (void)
{
    const char *shell;

    /* Check prefs first */
    shell = moo_prefs_get_string (SHELL_KEY);
    if (shell && shell[0])
        return g_strdup (shell);

    /* Try environment */
    shell = g_getenv ("SHELL");
    if (shell && shell[0])
        return g_strdup (shell);

    /* Try passwd */
#ifndef __WIN32__
    {
        struct passwd *pw = getpwuid (getuid ());
        if (pw && pw->pw_shell && pw->pw_shell[0])
            return g_strdup (pw->pw_shell);
    }
#endif

    return g_strdup ("/bin/sh");
}

static gboolean
shell_supports_pushd (const char *shell)
{
    const char *name = strrchr (shell, '/');
    name = name ? name + 1 : shell;
    return g_strcmp0 (name, "bash") == 0;
}

/* ================================================================ */
/* Terminal widget creation                                         */
/* ================================================================ */

/* Time of the last spawn, stored per terminal, so a shell that dies
 * immediately cannot be respawned in a tight loop. */
#define TERMINAL_SPAWN_TIME_KEY  "moo-terminal-spawn-time"
#define TERMINAL_MIN_SHELL_USEC  (200 * 1000)

static void
terminal_spawn (VteTerminal *term)
{
    char *shell = get_user_shell ();
    char *argv[] = { shell, NULL };

    /* Try spawn_async first (VTE >= 0.63), fall back to spawn_sync */
#if VTE_CHECK_VERSION(0, 63, 0)
    vte_terminal_spawn_async (term,
        VTE_PTY_DEFAULT, NULL, argv, NULL,
        G_SPAWN_DEFAULT, NULL, NULL, NULL,
        -1, NULL, NULL, NULL);
#elif VTE_CHECK_VERSION(0, 48, 0)
    vte_terminal_spawn_sync (term,
        VTE_PTY_DEFAULT, NULL, argv, NULL,
        G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL, NULL);
#else
    vte_terminal_fork_command_full (term,
        VTE_PTY_DEFAULT, NULL, argv, NULL,
        G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
#endif

    g_free (shell);

    {
        gint64 *stamp = g_new (gint64, 1);
        *stamp = g_get_monotonic_time ();
        g_object_set_data_full (G_OBJECT (term), TERMINAL_SPAWN_TIME_KEY,
                                stamp, g_free);
    }
}

static void
on_child_exited (VteTerminal *term, gint status, gpointer user_data)
{
    gint64 *spawned;
    gint64 now;

    (void)status;
    (void)user_data;

    /* The pane is being torn down: VTE emits "child-exited" when the PTY goes
       away, and respawning here leaks an orphan shell per split. */
    if (gtk_widget_in_destruction (GTK_WIDGET (term)))
        return;

    /* vte_terminal_spawn_async() reports spawn failure by emitting
       "child-exited", so an unspawnable shell -- a bad "Shell" preference,
       say -- would otherwise fork forever and wedge the UI. */
    now = g_get_monotonic_time ();
    spawned = (gint64 *) g_object_get_data (G_OBJECT (term),
                                            TERMINAL_SPAWN_TIME_KEY);

    if (spawned && now - *spawned < TERMINAL_MIN_SHELL_USEC)
    {
        char *shell = get_user_shell ();
        char *msg = g_strdup_printf (
            "\r\n[medit] '%s' exited immediately; not restarting it.\r\n"
            "[medit] Check the Shell preference or $SHELL.\r\n", shell);
        vte_terminal_feed (term, msg, -1);
        g_free (msg);
        g_free (shell);
        return;
    }

    vte_terminal_reset (term, TRUE, TRUE);
    terminal_spawn (term);
}

static VteTerminal *
create_terminal (void)
{
    VteTerminal *term;
    const char *cs_name;
    const TermColorScheme *cs;
    const char *font_name;
    int scrollback;

    term = VTE_TERMINAL (vte_terminal_new ());

    /* Color scheme */
    cs_name = moo_prefs_get_string (COLOR_SCHEME_KEY);
    cs = find_color_scheme (cs_name);
    apply_color_scheme (term, cs);

    /* Font */
    font_name = moo_prefs_get_string (FONT_KEY);
    if (font_name && font_name[0]) {
        PangoFontDescription *fd = pango_font_description_from_string (font_name);
        if (fd) {
            vte_terminal_set_font (term, fd);
            pango_font_description_free (fd);
        }
    }

    /* Scrollback */
    scrollback = moo_prefs_get_int (SCROLLBACK_KEY);
    if (scrollback <= 0)
        scrollback = 1000000;
    vte_terminal_set_scrollback_lines (term, scrollback);

    /* ── Fix PS1 duplication on resize ──────────────────────────────
     *
     * VTE rewraps scrollback (including the prompt line) when the
     * terminal width changes.  Bash/readline doesn't know lines have
     * been reflowed, so it reprints PS1 using stale geometry, causing
     * the prompt to duplicate and grow.
     *
     * For VTE < 0.58 we can simply disable rewrap-on-resize.
     * For VTE 0.58-0.60 the API is deprecated; for VTE >= 0.60 it is
     * a no-op.  In those cases we additionally inject shell-side
     * workarounds in create_terminal_box() after spawn.
     */
#if VTE_CHECK_VERSION(0, 36, 0)
    vte_terminal_set_rewrap_on_resize (term, FALSE);
#endif

    /* Size — use a minimal size request so that VTE does not fight
     * the GtkPaned layout during window resizes, which would cause
     * rapid SIGWINCH signals and repeated PS1 prompt reprints. */
    vte_terminal_set_size (term, vte_terminal_get_column_count (term), 10);
    gtk_widget_set_size_request (GTK_WIDGET (term), 10, 10);

    return term;
}

/* ================================================================ */
/* Suppress harmless GTK layout warnings during split               */
/* ================================================================ */

/*
 * During a terminal split, VTE's internal GtkScrolledWindow may
 * briefly receive a 1-pixel allocation before the GtkPaned divider
 * position is applied.  This causes GTK to emit "Negative content
 * width" warnings and "gtk_box_gadget_distribute: assertion
 * 'size >= 0' failed" criticals.  These are cosmetic and harmless,
 * so we filter them out to keep the console clean.
 *
 * The original GTK log handler is saved and called for all other
 * messages.
 */

static GLogWriterOutput
filtered_gtk_log_writer (GLogLevelFlags   log_level,
                         const GLogField *fields,
                         gsize            n_fields,
                         gpointer         user_data)
{
    gsize i;
    (void)user_data;

    for (i = 0; i < n_fields; i++) {
        if (g_strcmp0 (fields[i].key, "MESSAGE") == 0 &&
            fields[i].value != NULL) {
            const char *msg = (const char *)fields[i].value;
            if (strstr (msg, "Negative content width") != NULL)
                return G_LOG_WRITER_HANDLED;
            if (strstr (msg, "Negative content height") != NULL)
                return G_LOG_WRITER_HANDLED;
            if (strstr (msg, "gtk_box_gadget_distribute") != NULL)
                return G_LOG_WRITER_HANDLED;
        }
    }

    return g_log_writer_default (log_level, fields, n_fields, user_data);
}

static void
install_gtk_log_filter (void)
{
    static gboolean done = FALSE;

    if (done)
        return;
    done = TRUE;

    g_log_set_writer_func (filtered_gtk_log_writer, NULL, NULL);
}

/* ================================================================ */
/* Paned handle styling                                             */
/* ================================================================ */

/*
 * Apply a CSS tweak so the GtkPaned divider handles used for terminal
 * splits are wide enough to grab comfortably.  Called once at plugin
 * init time.  The min-width / min-height of 5px ensures visibility
 * even in themes where the default handle is 1-2 pixels.
 */
static void
install_paned_css (void)
{
    GtkCssProvider *provider;
    static gboolean done = FALSE;

    if (done)
        return;
    done = TRUE;

    provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider,
        "paned > separator {"
        "  min-width: 5px;"
        "  min-height: 5px;"
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen (
        gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
}

/* ================================================================ */
/* "cd to document directory" support                               */
/* ================================================================ */

static void
terminal_chdir (VteTerminal *term, const char *path, gboolean use_pushd)
{
    char *command;
    const char *cmd = use_pushd ? "pushd" : "cd";

    if (!path || !path[0])
        return;

    command = g_strdup_printf ("%s '%s'\n", cmd, path);
    vte_terminal_feed_child (term, command, -1);
    g_free (command);
}

static void
terminal_goto_file_dir (VteTerminal *term, gboolean use_pushd)
{
    MooEditor *editor;
    MooEdit   *doc;
    char      *filename, *dir;

    editor = moo_editor_instance ();
    if (!editor)
        return;

    doc = moo_editor_get_active_doc (editor);
    if (!doc)
        return;

    filename = moo_edit_get_filename (doc);
    if (!filename || !filename[0]) {
        g_free (filename);
        return;
    }

    dir = g_path_get_dirname (filename);
    terminal_chdir (term, dir, use_pushd);

    g_free (dir);
    g_free (filename);
}

static void
on_cd_activate (GtkMenuItem *item, VteTerminal *term)
{
    (void)item;
    terminal_goto_file_dir (term, FALSE);
}

static void
on_pushd_activate (GtkMenuItem *item, VteTerminal *term)
{
    (void)item;
    terminal_goto_file_dir (term, TRUE);
}

/* ================================================================ */
/* Ctrl+Scroll zoom                                                 */
/* ================================================================ */

static void
terminal_change_font_size (VteTerminal *term, gint delta)
{
    const PangoFontDescription *current;
    PangoFontDescription *fd;
    gint size;

    current = vte_terminal_get_font (term);
    if (!current)
        return;

    fd = pango_font_description_copy (current);
    size = pango_font_description_get_size (fd);

    size += delta * PANGO_SCALE;

    /* Clamp to a reasonable minimum (4pt) */
    if (size < 4 * PANGO_SCALE)
        size = 4 * PANGO_SCALE;

    pango_font_description_set_size (fd, size);
    vte_terminal_set_font (term, fd);
    pango_font_description_free (fd);
}

static gboolean
on_scroll_event (GtkWidget *widget, GdkEventScroll *event, gpointer data)
{
    VteTerminal *term = VTE_TERMINAL (widget);
    (void)data;

    /* Only handle Ctrl+scroll */
    if ((event->state & GDK_CONTROL_MASK) == 0)
        return FALSE;

    if (event->direction == GDK_SCROLL_UP) {
        terminal_change_font_size (term, 1);
        return TRUE;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        terminal_change_font_size (term, -1);
        return TRUE;
    }

    /* Handle smooth scrolling (trackpads) */
    if (event->direction == GDK_SCROLL_SMOOTH) {
        gdouble dx, dy;
        gdk_event_get_scroll_deltas ((GdkEvent *)event, &dx, &dy);
        if (dy < -0.5)
            terminal_change_font_size (term, 1);
        else if (dy > 0.5)
            terminal_change_font_size (term, -1);
        return TRUE;
    }

    return FALSE;
}

/* ================================================================ */
/* Context-menu callbacks                                           */
/* ================================================================ */

static void
on_copy_activate (GtkMenuItem *item, VteTerminal *term)
{
    (void)item;
#if VTE_CHECK_VERSION(0, 50, 0)
    vte_terminal_copy_clipboard_format (term, VTE_FORMAT_TEXT);
#else
    vte_terminal_copy_clipboard (term);
#endif
}

static void
on_paste_activate (GtkMenuItem *item, VteTerminal *term)
{
    (void)item;
    vte_terminal_paste_clipboard (term);
}

static void
on_font_activate (GtkMenuItem *item, VteTerminal *term)
{
    GtkWidget *dlg;
    (void)item;

    dlg = gtk_font_chooser_dialog_new ("Pick a Font",
            GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (term))));

    {
        const PangoFontDescription *old_font = vte_terminal_get_font (term);
        if (old_font)
            gtk_font_chooser_set_font_desc (GTK_FONT_CHOOSER (dlg), old_font);
    }

    if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_OK) {
        PangoFontDescription *fd = gtk_font_chooser_get_font_desc (GTK_FONT_CHOOSER (dlg));
        if (fd) {
            vte_terminal_set_font (term, fd);
            char *name = pango_font_description_to_string (fd);
            moo_prefs_set_string (FONT_KEY, name);
            g_free (name);
            pango_font_description_free (fd);
        }
    }

    gtk_widget_destroy (dlg);
}

static void
on_scheme_activate (GtkMenuItem *item, VteTerminal *term)
{
    const char *name = g_object_get_data (G_OBJECT (item), "scheme-name");
    const TermColorScheme *cs = find_color_scheme (name);
    apply_color_scheme (term, cs);
    moo_prefs_set_string (COLOR_SCHEME_KEY, name);
}

/* ================================================================ */
/* Split / Close logic                                              */
/* ================================================================ */

/*
 * Given a VteTerminal, walk up to find its immediate "terminal box"
 * (the hbox that holds terminal + scrollbar).
 */
static GtkWidget *
find_terminal_box (VteTerminal *term)
{
    /* The terminal's direct parent is the hbox */
    return gtk_widget_get_parent (GTK_WIDGET (term));
}

/*
 * Count how many VteTerminal widgets exist under a container
 * (recursive).  Used to decide whether "Close" is allowed.
 */
static gint
count_terminals (GtkWidget *widget)
{
    if (VTE_IS_TERMINAL (widget))
        return 1;

    if (GTK_IS_CONTAINER (widget)) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
        GList *l;
        gint n = 0;
        for (l = children; l; l = l->next)
            n += count_terminals (GTK_WIDGET (l->data));
        g_list_free (children);
        return n;
    }

    return 0;
}

/*
 * Find the top-level frame widget that holds the entire terminal
 * tree.  We walk up until we find a GtkFrame (the one registered
 * with the pane).
 */
static GtkWidget *
find_root_frame (GtkWidget *w)
{
    while (w) {
        if (GTK_IS_FRAME (w))
            return w;
        w = gtk_widget_get_parent (w);
    }
    return NULL;
}

/*
 * split_position_after_realize:
 *
 * Callback connected to the "realize" signal of a newly created GtkPaned.
 * At realize time the paned has received its final allocation from the
 * parent, so we can read back the actual pixel size and place the
 * divider exactly at the midpoint.  This guarantees a perfect 50/50
 * split regardless of how deeply nested the paned tree is.
 */
static void
split_position_after_realize (GtkWidget *paned, gpointer user_data)
{
    GtkAllocation alloc;
    GtkOrientation orientation;

    (void)user_data;

    gtk_widget_get_allocation (paned, &alloc);
    orientation = gtk_orientable_get_orientation (GTK_ORIENTABLE (paned));

    if (orientation == GTK_ORIENTATION_VERTICAL)
        gtk_paned_set_position (GTK_PANED (paned), alloc.height / 2);
    else
        gtk_paned_set_position (GTK_PANED (paned), alloc.width / 2);

    /* One-shot: disconnect after first realization */
    g_signal_handlers_disconnect_by_func (paned,
        G_CALLBACK (split_position_after_realize), NULL);
}

/*
 * do_split:
 *   orientation: GTK_ORIENTATION_VERTICAL   => split horizontally
 *                 (top/bottom — the *divider* is horizontal)
 *                GTK_ORIENTATION_HORIZONTAL => split vertically
 *                 (left/right — the *divider* is vertical)
 *
 * NOTE on naming convention matching Terminator:
 *   "Split Horizontally" = new terminal appears below  => vertical paned
 *   "Split Vertically"   = new terminal appears right  => horizontal paned
 */
static void
do_split (VteTerminal *term, GtkOrientation orientation)
{
    GtkWidget *term_box, *parent, *paned, *new_box;
    GtkAllocation alloc;

    term_box = find_terminal_box (term);
    g_return_if_fail (term_box != NULL);

    parent = gtk_widget_get_parent (term_box);
    g_return_if_fail (parent != NULL);

    /* Snapshot the current size of the area we are about to split.
     * This is used for the initial divider hint; the realize callback
     * will correct it to an exact 50/50 once layout is final. */
    gtk_widget_get_allocation (term_box, &alloc);

    /* Create the paned container */
    paned = gtk_paned_new (orientation);

    /* Reparent: remove term_box from parent, insert paned, then
     * put term_box into paned's child1 */
    g_object_ref (term_box);

    if (GTK_IS_PANED (parent)) {
        /* Determine if term_box is child1 or child2 */
        if (gtk_paned_get_child1 (GTK_PANED (parent)) == term_box) {
            gtk_container_remove (GTK_CONTAINER (parent), term_box);
            gtk_paned_pack1 (GTK_PANED (parent), paned, TRUE, TRUE);
        } else {
            gtk_container_remove (GTK_CONTAINER (parent), term_box);
            gtk_paned_pack2 (GTK_PANED (parent), paned, TRUE, TRUE);
        }
    } else {
        /* Parent is the frame's direct child (the initial hbox case)
         * or a box/frame.  For any generic GtkContainer: */
        gtk_container_remove (GTK_CONTAINER (parent), term_box);
        gtk_container_add (GTK_CONTAINER (parent), paned);
    }

    gtk_paned_pack1 (GTK_PANED (paned), term_box, TRUE, TRUE);
    g_object_unref (term_box);

    /* Create a new terminal box and put it in paned's child2 */
    new_box = create_terminal_box ();
    gtk_paned_pack2 (GTK_PANED (paned), new_box, TRUE, TRUE);

    /* Set an initial divider hint *before* show_all so GTK has a
     * reasonable target during the first allocation pass.  This avoids
     * the "Negative content width" warnings that occur when a child
     * is allocated a size of 1 pixel before layout is complete. */
    if (orientation == GTK_ORIENTATION_VERTICAL)
        gtk_paned_set_position (GTK_PANED (paned), alloc.height / 2);
    else
        gtk_paned_set_position (GTK_PANED (paned), alloc.width / 2);

    /* After the paned is realized and receives its true allocation,
     * re-set the position to an exact 50/50 split. */
    g_signal_connect (paned, "realize",
                      G_CALLBACK (split_position_after_realize), NULL);

    gtk_widget_show_all (paned);
}

static void
on_split_h_activate (GtkMenuItem *item, VteTerminal *term)
{
    (void)item;
    /* "Split Horizontally" => new terminal to the right => horizontal paned */
    do_split (term, GTK_ORIENTATION_HORIZONTAL);
}

static void
on_split_v_activate (GtkMenuItem *item, VteTerminal *term)
{
    (void)item;
    /* "Split Vertically" => new terminal below => vertical paned */
    do_split (term, GTK_ORIENTATION_VERTICAL);
}

static void
on_close_activate (GtkMenuItem *item, VteTerminal *term)
{
    GtkWidget *term_box, *parent, *root_frame, *sibling;

    (void)item;

    term_box = find_terminal_box (term);
    g_return_if_fail (term_box != NULL);

    root_frame = find_root_frame (term_box);

    /* Don't allow closing the very last terminal */
    if (root_frame && count_terminals (root_frame) <= 1)
        return;

    parent = gtk_widget_get_parent (term_box);
    g_return_if_fail (GTK_IS_PANED (parent));

    /* Find the sibling in the paned */
    if (gtk_paned_get_child1 (GTK_PANED (parent)) == term_box)
        sibling = gtk_paned_get_child2 (GTK_PANED (parent));
    else
        sibling = gtk_paned_get_child1 (GTK_PANED (parent));

    g_return_if_fail (sibling != NULL);

    /* Remove both children from the paned */
    g_object_ref (sibling);
    gtk_container_remove (GTK_CONTAINER (parent), term_box);
    gtk_container_remove (GTK_CONTAINER (parent), sibling);

    /* Now replace the paned with the sibling in the paned's parent */
    GtkWidget *grandparent = gtk_widget_get_parent (parent);
    g_return_if_fail (grandparent != NULL);

    if (GTK_IS_PANED (grandparent)) {
        if (gtk_paned_get_child1 (GTK_PANED (grandparent)) == parent) {
            gtk_container_remove (GTK_CONTAINER (grandparent), parent);
            gtk_paned_pack1 (GTK_PANED (grandparent), sibling, TRUE, TRUE);
        } else {
            gtk_container_remove (GTK_CONTAINER (grandparent), parent);
            gtk_paned_pack2 (GTK_PANED (grandparent), sibling, TRUE, TRUE);
        }
    } else {
        /* grandparent is the frame or some other simple container */
        gtk_container_remove (GTK_CONTAINER (grandparent), parent);
        gtk_container_add (GTK_CONTAINER (grandparent), sibling);
    }

    g_object_unref (sibling);
    gtk_widget_show_all (grandparent);

    /* term_box (and the VteTerminal inside it) are destroyed
     * automatically when removed from the container tree, since we
     * didn't take an extra ref on them. */
}

/* ================================================================ */
/* Active terminal highlight                                        */
/* ================================================================ */

/*
 * Draw a colored bar at the top of the focused terminal to indicate
 * which panel is active, similar to the active tab highlight.
 */
static gboolean
on_terminal_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;

    if (gtk_widget_has_focus (widget) || gtk_widget_is_focus (widget))
    {
        GdkRGBA highlight = {0.3, 0.6, 1.0, 0.8};
        int bar_height = 3;
        int width = gtk_widget_get_allocated_width (widget);

        cairo_save (cr);
        gdk_cairo_set_source_rgba (cr, &highlight);
        cairo_rectangle (cr, 0, 0, width, bar_height);
        cairo_fill (cr);
        cairo_restore (cr);
    }

    return FALSE;  /* propagate to let VTE draw its content */
}

/*
 * Queue a redraw on both the newly focused terminal and all siblings
 * so the highlight bar appears/disappears promptly on focus change.
 */
static void
redraw_all_terminals_in_frame (GtkWidget *w)
{
    if (VTE_IS_TERMINAL (w)) {
        gtk_widget_queue_draw (w);
    } else if (GTK_IS_CONTAINER (w)) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (w));
        GList *l;
        for (l = children; l; l = l->next)
            redraw_all_terminals_in_frame (GTK_WIDGET (l->data));
        g_list_free (children);
    }
}

static gboolean
on_terminal_focus_change (GtkWidget *widget, GdkEventFocus *event, gpointer data)
{
    GtkWidget *root;
    (void)event;
    (void)data;

    root = find_root_frame (widget);
    if (root)
        redraw_all_terminals_in_frame (root);
    else
        gtk_widget_queue_draw (widget);

    return FALSE;
}

/* ================================================================ */
/* Context menu                                                     */
/* ================================================================ */

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/*
 * Helper: create a menu item with an icon from the icon theme.
 * Uses deprecated GtkImageMenuItem but it still works in GTK3
 * and is the simplest way to get icons in popup menus.
 */
static GtkWidget *
menu_item_new_with_icon (const char *icon_name, const char *label)
{
    GtkWidget *item, *image;

    item = gtk_image_menu_item_new_with_label (label);

    if (icon_name) {
        image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_MENU);
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
    }

    return item;
}

G_GNUC_END_IGNORE_DEPRECATIONS

static gboolean
on_button_press (VteTerminal *term, GdkEventButton *event, gpointer data)
{
    GtkWidget *menu, *item, *submenu;
    guint i;
    (void)data;

    if (event->button != 3 || event->type != GDK_BUTTON_PRESS)
        return FALSE;

    menu = gtk_menu_new ();

    /* Copy */
    item = menu_item_new_with_icon ("edit-copy", "Copy");
    g_signal_connect (item, "activate", G_CALLBACK (on_copy_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    /* Paste */
    item = menu_item_new_with_icon ("edit-paste", "Paste");
    g_signal_connect (item, "activate", G_CALLBACK (on_paste_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                           gtk_separator_menu_item_new ());

    /* cd / pushd to current file directory */
    item = menu_item_new_with_icon ("folder-open", "Go to file directory");
    g_signal_connect (item, "activate", G_CALLBACK (on_cd_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    {
        char *user_shell = get_user_shell ();
        if (shell_supports_pushd (user_shell)) {
            item = menu_item_new_with_icon ("media-floppy", "Save current directory");
            g_signal_connect (item, "activate",
                              G_CALLBACK (on_pushd_activate), term);
            gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
        }
        g_free (user_shell);
    }

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                           gtk_separator_menu_item_new ());

    /* ---- Split / Close ---- */
    item = menu_item_new_with_icon ("object-flip-horizontal", "Split Horizontally");
    g_signal_connect (item, "activate", G_CALLBACK (on_split_h_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    item = menu_item_new_with_icon ("object-flip-vertical", "Split Vertically");
    g_signal_connect (item, "activate", G_CALLBACK (on_split_v_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    item = menu_item_new_with_icon ("window-close", "Close");
    g_signal_connect (item, "activate", G_CALLBACK (on_close_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    /* Grey out Close if this is the last terminal */
    {
        GtkWidget *root = find_root_frame (GTK_WIDGET (term));
        if (root && count_terminals (root) <= 1)
            gtk_widget_set_sensitive (item, FALSE);
    }

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                           gtk_separator_menu_item_new ());

    /* Font */
    item = menu_item_new_with_icon ("preferences-desktop-font", "Select Font...");
    g_signal_connect (item, "activate", G_CALLBACK (on_font_activate), term);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    /* Color Scheme submenu */
    item = menu_item_new_with_icon ("preferences-desktop-theme", "Color Scheme");
    submenu = gtk_menu_new ();
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);

    for (i = 0; i < N_COLOR_SCHEMES; i++) {
        GtkWidget *cs_item = gtk_menu_item_new_with_label (color_schemes[i].name);
        g_object_set_data (G_OBJECT (cs_item), "scheme-name",
                           (gpointer)color_schemes[i].name);
        g_signal_connect (cs_item, "activate",
                          G_CALLBACK (on_scheme_activate), term);
        gtk_menu_shell_append (GTK_MENU_SHELL (submenu), cs_item);
    }

    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    gtk_widget_show_all (menu);
    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *)event);

    return TRUE;
}

/* ================================================================ */
/* Terminal box: hbox holding VteTerminal + scrollbar                */
/* ================================================================ */

/*
 * Creates a self-contained terminal box: an hbox with a new
 * VteTerminal and its scrollbar, with shell spawned and
 * signals connected.
 */
static GtkWidget *
create_terminal_box (void)
{
    GtkWidget *hbox, *scrollbar;
    VteTerminal *term;

    term = create_terminal ();

    g_signal_connect (term, "child-exited",
                      G_CALLBACK (on_child_exited), NULL);
    g_signal_connect (term, "button-press-event",
                      G_CALLBACK (on_button_press), NULL);
    g_signal_connect (term, "scroll-event",
                      G_CALLBACK (on_scroll_event), NULL);
    g_signal_connect_after (term, "draw",
                            G_CALLBACK (on_terminal_draw), NULL);
    g_signal_connect (term, "focus-in-event",
                      G_CALLBACK (on_terminal_focus_change), NULL);
    g_signal_connect (term, "focus-out-event",
                      G_CALLBACK (on_terminal_focus_change), NULL);

    terminal_spawn (term);

    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start (GTK_BOX (hbox), GTK_WIDGET (term), TRUE, TRUE, 0);

    scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL,
                    gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (term)));
    gtk_box_pack_start (GTK_BOX (hbox), scrollbar, FALSE, FALSE, 0);

    return hbox;
}

/* ================================================================ */
/* Plugin structure                                                 */
/* ================================================================ */

typedef struct {
    MooWinPlugin parent;
    VteTerminal *terminal;     /* the initial terminal (for icon-title) */
    MooPane     *pane;
    gulong       icon_title_id;
} TerminalWindowPlugin;

static void
on_icon_title_changed (VteTerminal *term, gpointer data)
{
    TerminalWindowPlugin *plugin = data;
    const char *title;

    title = vte_terminal_get_icon_title (term);
    if (title && plugin->pane)
        moo_pane_set_frame_text (plugin->pane, title);
}

static gboolean
terminal_window_plugin_create (TerminalWindowPlugin *plugin)
{
    MooEditWindow *window;
    MooPaneLabel *label;
    GtkWidget *frame, *hbox;

    window = MOO_WIN_PLUGIN (plugin)->window;

    label = moo_pane_label_new (MOO_STOCK_TERMINAL, NULL,
                                _("Terminal"),
                                _("Terminal"));

    /* Create the initial terminal box */
    hbox = create_terminal_box ();

    /* Remember the first terminal for icon-title and chdir */
    {
        GList *children = gtk_container_get_children (GTK_CONTAINER (hbox));
        plugin->terminal = VTE_TERMINAL (children->data);
        g_list_free (children);
        /* Closing this split destroys the terminal while the plugin is still
           alive, so the cached pointer has to clear itself -- otherwise
           terminal_window_plugin_destroy() disconnects a handler on freed
           memory. */
        g_object_add_weak_pointer (G_OBJECT (plugin->terminal),
                                   (gpointer *) &plugin->terminal);
    }

    /* Build the top-level frame container (shadow none to avoid
     * a visible dashed border around the terminal area) */
    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);
    gtk_container_add (GTK_CONTAINER (frame), hbox);
    gtk_widget_show_all (frame);

    plugin->pane = moo_edit_window_add_pane (window,
        TERMINAL_PLUGIN_ID, frame, label, MOO_PANE_POS_BOTTOM);

    plugin->icon_title_id = g_signal_connect (plugin->terminal,
        "icon-title-changed", G_CALLBACK (on_icon_title_changed), plugin);

    on_icon_title_changed (plugin->terminal, plugin);

    moo_pane_label_free (label);

    return TRUE;
}

static void
terminal_window_plugin_destroy (TerminalWindowPlugin *plugin)
{
    MooEditWindow *window = MOO_WIN_PLUGIN (plugin)->window;

    if (plugin->terminal) {
        g_signal_handler_disconnect (plugin->terminal, plugin->icon_title_id);
        g_object_remove_weak_pointer (G_OBJECT (plugin->terminal),
                                      (gpointer *) &plugin->terminal);
        plugin->terminal = NULL;
    }

    moo_edit_window_remove_pane (window, TERMINAL_PLUGIN_ID);
    plugin->pane = NULL;
}

/* ================================================================ */
/* Plugin registration — follows ctags plugin pattern               */
/* ================================================================ */

typedef struct {
    MooPlugin parent;
} TerminalPlugin;

static gboolean
terminal_plugin_init (G_GNUC_UNUSED TerminalPlugin *plugin)
{
    /* Register preference keys */
    moo_prefs_new_key_string (COLOR_SCHEME_KEY, "Default");
    moo_prefs_new_key_string (SHELL_KEY, NULL);
    moo_prefs_new_key_string (FONT_KEY, NULL);
    moo_prefs_new_key_int (SCROLLBACK_KEY, 1000000);

    /* Filter out harmless GTK layout warnings from split resizes */
    install_gtk_log_filter ();

    /* Make paned divider handles easy to grab */
    install_paned_css ();

    return TRUE;
}

static void
terminal_plugin_deinit (G_GNUC_UNUSED TerminalPlugin *plugin)
{
}

MOO_PLUGIN_DEFINE_INFO (terminal,
                        "Terminal",
                        "Embedded terminal emulator",
                        "Yevgen Muntyan <emuntyan@users.sourceforge.net>",
                        MOO_VERSION)

MOO_WIN_PLUGIN_DEFINE (Terminal, terminal)

MOO_PLUGIN_DEFINE (Terminal, terminal,
                   NULL, NULL, NULL, NULL, NULL,
                   terminal_window_plugin_get_type (),
                   0)

gboolean
_moo_terminal_plugin_init (void)
{
    MooPluginParams params = { TRUE, TRUE };
    return moo_plugin_register (TERMINAL_PLUGIN_ID,
                                terminal_plugin_get_type (),
                                &terminal_plugin_info,
                                &params);
}