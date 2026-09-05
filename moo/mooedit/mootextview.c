/*
 *   mootextview.c
 *
 *   Copyright (C) 2004-2010 by Yevgen Muntyan <emuntyan@users.sourceforge.net>
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with medit.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * class:MooTextView: (parent GtkTextView) (constructable): text view object
 **/

#include "mooedit/mooedit-accels.h"
#define SAFE_GDK_WINDOW_GET_WIDTH(w) ((w) && GDK_IS_WINDOW(w) ? gdk_window_get_width(w) : 0)
#include "mooedit/mootextview-private.h"
#include "mooedit/moofold.h"
#include "mooedit/mootextview.h"
#include "mooedit/mootextbuffer.h"

/* LL_EXTERN_DECL */
extern void moo_ll_apply_range (GtkTextBuffer *buffer, int first_line, int last_line);
#include "mooedit/mootextfind.h"
#include "mooedit/mootext-private.h"
#include "mooedit/mooeditprefs.h"
#include "mooedit/moolangmgr.h"
#include "mooedit/mooeditwindow.h"
#include "mooedit/mooedit.h"
#include "marshals.h"
#include "mooutils/mooutils-misc.h"
#include "mooutils/mooundo.h"
#include "mooutils/mooeditops.h"
#include "mooutils/mooentry.h"
#include "mooutils/mooi18n.h"
#include "mooutils/mooatom.h"
#include "mooutils/mootype-macros.h"
#include "mooutils/moocompat.h"
#include <math.h>
#include "mooutils/mooutils-gobject.h"
#include "mooedit/mooquicksearch-gxml.h"
#include <gtk/gtk.h>
#include "mooutils/moo-gtk3-compat.h"
#include <mooglib/moo-glib.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

#define LIGHT_BLUE "#EEF6FF"
#define BOOL_CMP(b1,b2) ((b1 && b2) || (!b1 && !b2))
#define UPDATE_PRIORITY (GTK_TEXT_VIEW_PRIORITY_VALIDATE - 5)

#define MIN_LINE_MARK_WIDTH 6
#define DEFAULT_EXPANDER_SIZE 12
#define EXPANDER_PAD 1

static GtkTextWindowType window_types[4] = {
    GTK_TEXT_WINDOW_LEFT,
    GTK_TEXT_WINDOW_RIGHT,
    GTK_TEXT_WINDOW_TOP,
    GTK_TEXT_WINDOW_BOTTOM
};

static const GtkTargetEntry text_view_target_table[] = {
    { (char*) "GTK_TEXT_BUFFER_CONTENTS", GTK_TARGET_SAME_APP, 0 }
};

static void     undo_ops_iface_init         (MooUndoOpsIface    *iface);
static GObject *moo_text_view_constructor   (GType               type,
                                             guint               n_construct_properties,
                                             GObjectConstructParam *construct_param);
static void     moo_text_view_dispose       (GObject            *object);
static void     moo_text_view_finalize      (GObject            *object);

static void     moo_text_view_set_property  (GObject            *object,
                                             guint               prop_id,
                                             const GValue       *value,
                                             GParamSpec         *pspec);
static void     moo_text_view_get_property  (GObject            *object,
                                             guint               prop_id,
                                             GValue             *value,
                                             GParamSpec         *pspec);

static void     moo_text_view_realize       (GtkWidget          *widget);
static gboolean moo_text_view_scroll_event (GtkWidget *widget, GdkEventScroll *event);
static void     moo_text_view_unrealize     (GtkWidget          *widget);
static gboolean moo_text_view_expose        (GtkWidget          *widget,
                                             cairo_t            *cr);
static void     moo_text_view_style_set     (GtkWidget          *widget,
                                             GtkStyle           *previous_style);
static void     moo_text_view_size_request  (GtkWidget          *widget,
                                             GtkRequisition     *requisition);
static void     moo_text_view_get_preferred_width (GtkWidget    *widget,
                                                   gint         *minimum,
                                                   gint         *natural);
static void     moo_text_view_get_preferred_height (GtkWidget   *widget,
                                                    gint        *minimum,
                                                    gint        *natural);
static void     moo_text_view_size_allocate (GtkWidget          *widget,
                                             GtkAllocation      *allocation);

static void     moo_text_view_remove        (GtkContainer       *container,
                                             GtkWidget          *child);

/* ---- Long-line: click to reveal, wrap-mode tracking ---- */
#define MOO_LL_TAG "moo-ll-hidden"
#define MOO_LL_MARKER "moo-ll-marker"

extern void moo_ll_apply (GtkTextBuffer *buffer);
/* Box selection text extraction (defined in mootextview-input.c) */
extern char *box_sel_get_text (GtkTextView *tv, int ax, int ay, int bx, int by);
extern int box_sel_visual_col_at_x (GtkTextView *tv, int line, int buf_x,
                                    gboolean right_bound);
extern void box_sel_clear (MooTextView *view);
extern void box_sel_delete (MooTextView *view);
extern void moo_ll_remove_all (GtkTextBuffer *buffer);
extern void moo_ll_reveal_line (GtkTextBuffer *buffer, int line);
extern void moo_ll_apply_range (GtkTextBuffer *buffer, int first_line, int last_line);

static gboolean
moo_ll_click_handler (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    GtkTextView *tv = GTK_TEXT_VIEW (widget);
    GtkTextBuffer *buf;
    GtkTextTagTable *table;
    GtkTextTag *htag;
    GtkTextIter iter;
    int bx, by, line;

    (void)data;

    if (event->button != 1 || event->type != GDK_BUTTON_PRESS)
        return FALSE;

    buf = gtk_text_view_get_buffer (tv);
    table = gtk_text_buffer_get_tag_table (buf);
    htag = gtk_text_tag_table_lookup (table, MOO_LL_TAG);
    if (!htag)
        return FALSE;

    /* Convert click coords to buffer coords */
    gtk_text_view_window_to_buffer_coords (tv, GTK_TEXT_WINDOW_TEXT,
        (int)event->x, (int)event->y, &bx, &by);
    gtk_text_view_get_iter_at_location (tv, &iter, bx, by);

    line = gtk_text_iter_get_line (&iter);

    /* Check if this line has any hidden (truncated) region */
    {
        GtkTextIter scan;
        gtk_text_buffer_get_iter_at_line (buf, &scan, line);
        if (!gtk_text_iter_forward_to_tag_toggle (&scan, htag))
            return FALSE;
        if (gtk_text_iter_get_line (&scan) != line)
            return FALSE;
    }

    /* Compute where the "..." marker is drawn and check if click hits it.
     * This mirrors the logic in moo_text_view_draw_long_line_markers:
     * - marker_x = position after last visible char on the line
     * - if marker_x is off-screen, pin to right edge of viewport
     * - marker width = text width of " ..." + 4px padding */
    {
        GtkTextIter line_start, tag_start, last_vis;
        GdkRectangle char_rect;
        PangoLayout *layout;
        int win_x, win_y, right_edge;
        int marker_x, marker_w, marker_h;
        int click_wx, click_wy;

        gtk_text_buffer_get_iter_at_line (buf, &line_start, line);
        tag_start = line_start;
        if (!gtk_text_iter_forward_to_tag_toggle (&tag_start, htag))
            return FALSE;
        if (gtk_text_iter_get_line (&tag_start) != line)
            return FALSE;
        if (gtk_text_iter_get_line_offset (&tag_start) == 0)
            return FALSE;

        /* Get position after last visible character (just before hidden starts) */
        last_vis = tag_start;
        gtk_text_iter_backward_char (&last_vis);
        gtk_text_view_get_iter_location (tv, &last_vis, &char_rect);
        gtk_text_view_buffer_to_window_coords (tv, GTK_TEXT_WINDOW_TEXT,
            char_rect.x + char_rect.width, char_rect.y,
            &win_x, &win_y);

        /* LL_RIGHT_EDGE_FIX — use actual window width */
        right_edge = gdk_window_get_width (
            gtk_text_view_get_window (tv, GTK_TEXT_WINDOW_TEXT));

        /* Measure marker text */
        layout = gtk_widget_create_pango_layout (widget, " ...");
        pango_layout_get_pixel_size (layout, &marker_w, &marker_h);
        g_object_unref (layout);
        marker_w += 4;  /* padding, matches draw function */

        /* Compute marker_x: same logic as draw function */
        if (win_x >= 0 && win_x + marker_w <= right_edge)
            marker_x = win_x;
        else
            marker_x = right_edge - marker_w;

        if (marker_x < 0)
            marker_x = 0;

        /* Convert click position to window coords */
        gtk_text_view_buffer_to_window_coords (tv, GTK_TEXT_WINDOW_TEXT,
            bx, by, &click_wx, &click_wy);

        /* Hit test: check if click is within the marker rectangle */
        if (click_wx < marker_x || click_wx > marker_x + marker_w)
            return FALSE;
        if (click_wy < win_y || click_wy > win_y + char_rect.height)
            return FALSE;
    }

    /* Click is on the "..." marker — reveal next chunk */
    {
        GtkTextIter ls, le, hidden_start, new_hide_start;
        int old_trunc, line_len, new_trunc;

        gtk_text_buffer_get_iter_at_line (buf, &ls, line);
        le = ls;
        if (!gtk_text_iter_ends_line (&le))
            gtk_text_iter_forward_to_line_end (&le);

        /* Find where the hidden tag starts on this line */
        hidden_start = ls;
        if (!gtk_text_iter_forward_to_tag_toggle (&hidden_start, htag))
            return FALSE;
        if (gtk_text_iter_get_line (&hidden_start) != line)
            return FALSE;

        old_trunc = gtk_text_iter_get_line_offset (&hidden_start);
        line_len = gtk_text_iter_get_line_offset (&le);

        /* Remove the hidden tag from this line entirely */
        gtk_text_buffer_remove_tag (buf, htag, &ls, &le);

        /* New truncation point: reveal 4096 more chars */
        new_trunc = old_trunc + 4096;

        if (new_trunc < line_len)
        {
            /* Re-apply hidden tag from new truncation point to line end */
            new_hide_start = ls;
            gtk_text_iter_set_line_offset (&new_hide_start, new_trunc);
            gtk_text_buffer_apply_tag (buf, htag, &new_hide_start, &le);
        }

        /* Scroll to the new boundary or end of line */
        {
            GtkTextIter scroll_to;
            gtk_text_buffer_get_iter_at_line (buf, &scroll_to, line);
            if (new_trunc < line_len)
                gtk_text_iter_set_line_offset (&scroll_to,
                    new_trunc > 0 ? new_trunc - 1 : 0);
            else
            {
                if (!gtk_text_iter_ends_line (&scroll_to))
                    gtk_text_iter_forward_to_line_end (&scroll_to);
            }
            gtk_text_buffer_place_cursor (buf, &scroll_to);
            gtk_text_view_scroll_to_iter (tv, &scroll_to, 0.05, FALSE, 1.0, 0.0);
        }
    }

    gtk_widget_queue_draw (GTK_WIDGET (tv));
    return TRUE;
}

/* After text is inserted, apply long-line truncation to affected lines */
static void
moo_ll_after_insert (GtkTextBuffer *buffer,
                     GtkTextIter   *location,
                     gchar         *text,
                     gint           len,
                     gpointer       user_data)
{
    extern void moo_ll_apply_range (GtkTextBuffer *buf, int first, int last);
    GtkTextIter start_iter;
    int first_line, last_line;

    (void)user_data;

    if (!GPOINTER_TO_INT (g_object_get_data (G_OBJECT (buffer), "moo-nowrap-mode")))
        return;

    /* location points to end of inserted text */
    last_line = gtk_text_iter_get_line (location);
    start_iter = *location;
    gtk_text_iter_backward_chars (&start_iter, (int) g_utf8_strlen (text, len));
    first_line = gtk_text_iter_get_line (&start_iter);

    moo_ll_apply_range (buffer, first_line, last_line);
}

static void
moo_ll_wrap_changed (GObject *obj, GParamSpec *ps, gpointer data)
{
    GtkTextView *tv = GTK_TEXT_VIEW (obj);
    GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
    GtkWrapMode wm = gtk_text_view_get_wrap_mode (tv);
    (void)ps; (void)data;

    g_object_set_data (G_OBJECT (buf), "moo-nowrap-mode",
                       GINT_TO_POINTER (wm == GTK_WRAP_NONE ? 1 : 0));

    if (wm == GTK_WRAP_NONE)
        moo_ll_apply (buf);
    else
        moo_ll_remove_all (buf);
}



static void     moo_text_view_copy_clipboard (GtkTextView       *text_view);
static void     moo_text_view_box_copy (MooTextView       *view);
static void     moo_text_view_cut_clipboard (GtkTextView        *text_view);
static void     moo_text_view_paste_clipboard (GtkTextView      *text_view);
static void     moo_text_view_populate_popup(GtkTextView        *text_view,
                                             GtkMenu            *menu);

static void     moo_text_view_apply_style_scheme (MooTextView   *view,
                                             MooTextStyleScheme *scheme);

static void     invalidate_gcs              (MooTextView        *view);
static void     update_gcs                  (MooTextView        *view);
static void     update_tab_width            (MooTextView        *view);
static void     invalidate_right_margin     (MooTextView        *view);
static void     update_right_margin         (MooTextView        *view);

static GtkTextBuffer *get_buffer            (MooTextView        *view);
static MooTextBuffer *get_moo_buffer        (MooTextView        *view);
static GtkTextMark *get_insert              (MooTextView        *view);

static void     cursor_moved                (MooTextView        *view,
                                             GtkTextIter        *where);
static void     proxy_prop_notify           (MooTextView        *view,
                                             GParamSpec         *pspec);
static void     proxy_notify_can_undo_redo  (MooTextView        *view,
                                             GParamSpec         *pspec);

static void     find_word_at_cursor         (MooTextView        *view,
                                             gboolean            forward);
static void     find_interactive            (MooTextView        *view);
static void     replace_interactive         (MooTextView        *view);
static void     find_next_interactive       (MooTextView        *view);
static void     find_prev_interactive       (MooTextView        *view);
static void     goto_line_interactive       (MooTextView        *view);
static gboolean start_quick_search          (MooTextView        *view);
static void     moo_text_view_stop_quick_search (MooTextView    *view);

static void     insert_text_cb              (MooTextView        *view,
                                             GtkTextIter        *iter,
                                             gchar              *text,
                                             gint                len);
static gboolean moo_text_view_char_inserted (MooTextView        *view,
                                             GtkTextIter        *where,
                                             const char         *character);
static void     moo_text_view_delete_selection (MooTextView     *view);

static MooTextCursor moo_text_view_get_text_cursor
                                            (MooTextView        *view,
                                             int                 x,
                                             int                 y);

static void     set_manage_clipboard        (MooTextView        *view,
                                             gboolean            manage);
static void     selection_changed           (MooTextView        *view,
                                             MooTextBuffer      *buffer);
static void     highlight_updated           (GtkTextView        *view,
                                             const GtkTextIter  *start,
                                             const GtkTextIter  *end);

static void     set_draw_whitespace         (MooTextView        *view,
                                             MooDrawWsFlags      flags);

static void     buffer_changed              (MooTextView        *view);
static void     update_left_margin          (MooTextView        *view);
static void     moo_text_view_draw_layer    (GtkTextView        *text_view,
                                             GtkTextViewLayer    layer,
                                             cairo_t            *cr);
static void     draw_left_margin            (MooTextView        *view,
                                             cairo_t *cr_param);
static void     draw_marks_background       (MooTextView        *view,
                                             cairo_t *cr_param);
static gboolean update_n_lines_idle         (MooTextView        *view);

static void     add_line_mark               (MooTextView        *view,
                                             MooLineMark        *mark);
static void     remove_line_mark            (MooTextView        *view,
                                             MooLineMark        *mark);
static void     line_mark_added             (MooTextView        *view,
                                             MooLineMark        *mark);
static void     line_mark_deleted           (MooTextView        *view,
                                             MooLineMark        *mark);
static void     line_mark_changed           (MooTextView        *view,
                                             MooLineMark        *mark);
static void     line_mark_moved             (MooTextView        *view,
                                             MooLineMark        *mark);
static void     set_show_line_marks         (MooTextView        *view,
                                             gboolean            show);
static void     fold_added                  (MooTextView        *view,
                                             MooFold            *fold);
static void     fold_deleted                (MooTextView        *view);
static void     fold_toggled                (MooTextView        *view,
                                             MooFold            *fold);
static void     set_enable_folding          (MooTextView        *view,
                                             gboolean            show);

static gboolean text_iter_forward_visible_line (MooTextView     *view,
                                             GtkTextIter        *iter,
                                             int                *line);
static int      get_border_window_size      (GtkTextView        *text_view,
                                             GtkTextWindowType   type);


enum {
    DELETE_SELECTION,
    FIND_INTERACTIVE,
    FIND_WORD_AT_CURSOR,
    FIND_NEXT_INTERACTIVE,
    FIND_PREV_INTERACTIVE,
    REPLACE_INTERACTIVE,
    GOTO_LINE_INTERACTIVE,
    CURSOR_MOVED,
    CHAR_INSERTED,
    LINE_MARK_CLICKED,
    START_QUICK_SEARCH,
    UNDO,
    REDO,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

/* ======================================================================
 * FOLD VISUALS — ellipsis, header highlight, vertical guide lines
 * ====================================================================== */
/* FOLD_VISUALS_DRAW patched */

#include "mooedit/moofold.h"
#include "mooedit/mootext-private.h"
#include "mooedit/mootextbuffer.h"

static void
_moo_text_view_init_fold_visuals (MooTextView *view)
{
    view->priv->fold_header_bg.red   = 0.20;
    view->priv->fold_header_bg.green = 0.20;
    view->priv->fold_header_bg.blue  = 0.30;
    view->priv->fold_header_bg.alpha = 0.30;

    view->priv->fold_ellipsis_fg.red   = 0.60;
    view->priv->fold_ellipsis_fg.green = 0.60;
    view->priv->fold_ellipsis_fg.blue  = 0.65;
    view->priv->fold_ellipsis_fg.alpha = 1.0;

    view->priv->fold_guide_color.red   = 0.40;
    view->priv->fold_guide_color.green = 0.40;
    view->priv->fold_guide_color.blue  = 0.40;
    view->priv->fold_guide_color.alpha = 0.35;
}

static void
_moo_text_view_ensure_fold_header_tag (MooTextView *view)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    GtkTextTagTable *table;

    if (!buffer)
        return;

    table = gtk_text_buffer_get_tag_table (buffer);

    if (!gtk_text_tag_table_lookup (table, "moo-fold-header"))
    {
        view->priv->fold_header_tag = gtk_text_buffer_create_tag (
            buffer, "moo-fold-header",
            "paragraph-background", "#2a2a3a",
            NULL);
        /* Give it high priority so it shows over syntax tags */
        gtk_text_tag_set_priority (view->priv->fold_header_tag,
            gtk_text_tag_table_get_size (table) - 1);
    }
    else
    {
        view->priv->fold_header_tag =
            gtk_text_tag_table_lookup (table, "moo-fold-header");
    }
}

/* Called when fold-toggled is emitted on the buffer */

/* LL_FOLD_TOGGLE_FIX — manages ll truncation markers during fold/unfold */
static void
_moo_text_view_on_fold_toggled (MooTextBuffer  *buffer,
                                MooFold        *fold,
                                MooTextView    *view)
{
    GtkTextIter start, end;
    GtkTextBuffer *buf;
    int fold_line, fold_end_line;

    if (_moo_fold_is_deleted (fold))
        return;

    buf = GTK_TEXT_BUFFER (buffer);
    fold_line = _moo_fold_get_start (fold);
    fold_end_line = _moo_fold_get_end (fold);

    /* Apply/remove fold header highlight tag */
    gtk_text_buffer_get_iter_at_line (buf, &start, fold_line);
    end = start;
    gtk_text_iter_forward_line (&end);

    _moo_text_view_ensure_fold_header_tag (view);

    if (view->priv->fold_header_tag)
    {
        if (fold->collapsed)
            gtk_text_buffer_apply_tag (buf, view->priv->fold_header_tag,
                                       &start, &end);
        else
            gtk_text_buffer_remove_tag (buf, view->priv->fold_header_tag,
                                        &start, &end);
    }

    /* Long-line truncation marker management:
     * When collapsing: remove ll-hidden tag from folded lines so
     *   stale truncation markers don't paint on invisible lines.
     * When expanding: re-apply ll truncation so markers reappear. */
    if (fold->collapsed)
    {
        GtkTextTagTable *table = gtk_text_buffer_get_tag_table (buf);
        GtkTextTag *ll_tag = gtk_text_tag_table_lookup (table, "moo-ll-hidden");

        if (ll_tag && fold_end_line > fold_line + 1)
        {
            GtkTextIter ls, le;
            gtk_text_buffer_get_iter_at_line (buf, &ls, fold_line + 1);
            gtk_text_buffer_get_iter_at_line (buf, &le, fold_end_line);
            if (!gtk_text_iter_ends_line (&le))
                gtk_text_iter_forward_to_line_end (&le);
            gtk_text_buffer_remove_tag (buf, ll_tag, &ls, &le);
        }
    }
    else
    {
        if (fold_end_line > fold_line + 1)
            moo_ll_apply_range (buf, fold_line + 1, fold_end_line - 1);
    }

    /* Redraw only the fold region */
    {
        GtkTextView *text_view = GTK_TEXT_VIEW (view);
        GdkWindow *text_window;
        GtkTextIter tmp_iter;
        int line_y, line_h;
        GdkRectangle redraw_rect;
        int wy_start, wy_end;

        text_window = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_TEXT);
        if (!text_window)
        {
            gtk_widget_queue_draw (GTK_WIDGET (view));
            return;
        }

        gtk_text_buffer_get_iter_at_line (buf, &tmp_iter, fold_line);
        gtk_text_view_get_line_yrange (text_view, &tmp_iter, &line_y, &line_h);
        gtk_text_view_buffer_to_window_coords (text_view,
                                                GTK_TEXT_WINDOW_TEXT,
                                                0, line_y,
                                                NULL, &wy_start);

        gtk_text_buffer_get_iter_at_line (buf, &tmp_iter, fold_end_line);
        gtk_text_view_get_line_yrange (text_view, &tmp_iter, &line_y, &line_h);
        gtk_text_view_buffer_to_window_coords (text_view,
                                                GTK_TEXT_WINDOW_TEXT,
                                                0, line_y + line_h,
                                                NULL, &wy_end);

        redraw_rect.x = 0;
        redraw_rect.y = wy_start;
        redraw_rect.width = gdk_window_get_width (text_window);
        redraw_rect.height = wy_end - wy_start + 1;

        gdk_window_invalidate_rect (text_window, &redraw_rect, TRUE);

        {
            GdkWindow *left_window =
                gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_LEFT);
            if (left_window)
            {
                redraw_rect.width = gdk_window_get_width (left_window);
                gdk_window_invalidate_rect (left_window, &redraw_rect, TRUE);
            }
        }
    }
}


/* Draw " ..." after the end of text on each collapsed fold header line */

static void
_moo_text_view_draw_fold_ellipsis (MooTextView *view, cairo_t *cr)
{
    GtkTextView *text_view = GTK_TEXT_VIEW (view);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (text_view);
    MooTextBuffer *mbuf;
    GdkRectangle visible_rect;
    GtkTextIter vis_iter;
    int first_line, last_line, total_lines;
    GSList *folds, *l;
    PangoLayout *layout;
    PangoFontDescription *font_desc;

    if (!MOO_IS_TEXT_BUFFER (buffer))
        return;

    mbuf = MOO_TEXT_BUFFER (buffer);
    total_lines = gtk_text_buffer_get_line_count (buffer);
    if (total_lines < 2)
        return;

    gtk_text_view_get_visible_rect (text_view, &visible_rect);
    gtk_text_view_get_line_at_y (text_view, &vis_iter, visible_rect.y, NULL);
    first_line = gtk_text_iter_get_line (&vis_iter);
    gtk_text_view_get_line_at_y (text_view, &vis_iter,
                                  visible_rect.y + visible_rect.height, NULL);
    last_line = gtk_text_iter_get_line (&vis_iter);
    if (last_line >= total_lines)
        last_line = total_lines - 1;
    if (first_line > last_line)
        return;

    folds = moo_text_buffer_get_folds_in_range (mbuf, first_line, last_line);
    if (!folds)
        return;

    /* Create ellipsis layout once */
    layout = gtk_widget_create_pango_layout (GTK_WIDGET (view),
                                              " \xe2\x80\xa6"); /* " …" */
    font_desc = pango_font_description_copy (
        pango_context_get_font_description (
            gtk_widget_get_pango_context (GTK_WIDGET (view))));
    pango_font_description_set_style (font_desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description (layout, font_desc);

    cairo_save (cr);
    gdk_cairo_set_source_rgba (cr, &view->priv->fold_ellipsis_fg);

    for (l = folds; l != NULL; l = l->next)
    {
        MooFold *fold = (MooFold *) l->data;
        int fold_line;
        int chars_in_line;
        GtkTextIter line_start_iter, line_end_iter;
        GdkRectangle loc;
        int win_x, win_y;

        if (!fold->collapsed || _moo_fold_is_deleted (fold))
            continue;

        fold_line = _moo_fold_get_start (fold);

        /* --- Guard 1: skip very long lines entirely --- */
        gtk_text_buffer_get_iter_at_line (buffer, &line_start_iter, fold_line);
        chars_in_line = gtk_text_iter_get_chars_in_line (&line_start_iter);
        if (chars_in_line > 300)
            continue;

        /* Find end of text on this line */
        line_end_iter = line_start_iter;
        if (!gtk_text_iter_ends_line (&line_end_iter))
            gtk_text_iter_forward_to_line_end (&line_end_iter);

        /* Get pixel position of line end */
        gtk_text_view_get_iter_location (text_view, &line_end_iter, &loc);

        /* --- Guard 2: if line end is past the right edge of the
         *     visible area, the line is visually truncated.
         *     Don't show ellipsis — it would overlap or confuse. --- */
        if (loc.x + loc.width > visible_rect.x + visible_rect.width)
            continue;

        /* Convert to window coords and draw */
        gtk_text_view_buffer_to_window_coords (text_view,
                                                GTK_TEXT_WINDOW_TEXT,
                                                loc.x + loc.width,
                                                loc.y,
                                                &win_x, &win_y);
        cairo_move_to (cr, win_x + 4.0, win_y);
        pango_cairo_show_layout (cr, layout);
    }

    cairo_restore (cr);
    pango_font_description_free (font_desc);
    g_object_unref (layout);
    g_slist_free (folds);
}

/* Draw vertical indent guide lines for expanded folds */
static void
_moo_text_view_draw_fold_guides (MooTextView *view, cairo_t *cr)
{
    GtkTextView *text_view = GTK_TEXT_VIEW (view);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer (text_view);
    MooTextBuffer *mbuf;
    GdkRectangle visible_rect;
    GtkTextIter vis_iter;
    int first_line, last_line, total_lines;
    GSList *folds, *l;
    gboolean is_end_lang = FALSE;

    if (!MOO_IS_TEXT_BUFFER (buffer))
        return;

    mbuf = MOO_TEXT_BUFFER (buffer);
    total_lines = gtk_text_buffer_get_line_count (buffer);
    if (total_lines < 2)
        return;

    /* End-based languages (matlab/octave/pascal) write the block opener
     * directly on start_line — there is no continuation-line story for
     * scan-back to disentangle.  Anchoring the guide to start_line is
     * therefore both correct and faster, and avoids the overlapping /
     * offset guides observed on multi-block matlab classes. */
    {
        MooLang   *lang    = moo_text_buffer_get_lang (mbuf);
        const char *lang_id = _moo_lang_id (lang);
        is_end_lang = lang_id && (strcmp (lang_id, "matlab") == 0 ||
                                  strcmp (lang_id, "octave") == 0 ||
                                  strcmp (lang_id, "pascal") == 0);
    }

    gtk_text_view_get_visible_rect (text_view, &visible_rect);
    gtk_text_view_get_line_at_y (text_view, &vis_iter, visible_rect.y, NULL);
    first_line = gtk_text_iter_get_line (&vis_iter);
    gtk_text_view_get_line_at_y (text_view, &vis_iter,
                                  visible_rect.y + visible_rect.height, NULL);
    last_line = gtk_text_iter_get_line (&vis_iter);
    if (last_line >= total_lines)
        last_line = total_lines - 1;

    /* Get ALL folds (from line 0) so we catch folds starting above viewport */
    folds = moo_text_buffer_get_folds_in_range (mbuf, 0, last_line);
    if (!folds)
        return;

    cairo_save (cr);
    gdk_cairo_set_source_rgba (cr, &view->priv->fold_guide_color);
    cairo_set_line_width (cr, 2.0);

    for (l = folds; l != NULL; l = l->next)
    {
        MooFold *fold = (MooFold *) l->data;
        int start_line, end_line;
        GtkTextIter iter;
        GdkRectangle start_loc, end_loc, iloc;
        int guide_px;
        int win_x, y_top, y_bot;
        gunichar ch;

        if (_moo_fold_is_deleted (fold) || fold->collapsed)
            continue;

        start_line = _moo_fold_get_start (fold);
        end_line   = _moo_fold_get_end (fold);

        /* Skip folds entirely outside the viewport */
        if (end_line < first_line || start_line > last_line)
            continue;

        /* Align guide with the logical "owner" of the block.
         *
         * Problem: for functions with multi-line parameter lists the { falls
         * on the last parameter line which may be deeply indented, causing
         * the guide to cut through the function body:
         *
         *     void func (int a,         <- indent 0  (owner)
         *                int b) {       <- indent 15 (start_line)
         *         body;                 <- indent 4
         *     }
         *
         * Fix: measure start_line's leading-whitespace depth; scan backward
         * through continuation lines (same or deeper indent) until a line
         * with strictly less indentation is found — that is the owner.
         * Only one gtk_text_view_get_iter_location call is made, for the
         * winning line, to keep drawing fast.
         */
        {
            int start_ws, best_ws, guide_line, scan_line;

            /* Count leading whitespace on start_line */
            gtk_text_buffer_get_iter_at_line (buffer, &iter, start_line);
            start_ws = 0;
            while (!gtk_text_iter_ends_line (&iter))
            {
                ch = gtk_text_iter_get_char (&iter);
                if (ch != ' ' && ch != '\t') break;
                start_ws++;
                gtk_text_iter_forward_char (&iter);
            }

            /* Skip fold if start line is all whitespace */
            if (gtk_text_iter_ends_line (&iter))
                continue;

            best_ws    = start_ws;
            guide_line = start_line;

            /* End-based languages don't have C-style continuation-line
             * function signatures, so skip the scan-back and anchor the
             * guide to start_line directly. */
            for (scan_line = start_line - 1;
                 !is_end_lang && scan_line >= 0 && (start_line - scan_line) <= 50;
                 scan_line--)
            {
                int scan_ws = 0;
                gtk_text_buffer_get_iter_at_line (buffer, &iter, scan_line);
                while (!gtk_text_iter_ends_line (&iter))
                {
                    ch = gtk_text_iter_get_char (&iter);
                    if (ch != ' ' && ch != '\t') break;
                    scan_ws++;
                    gtk_text_iter_forward_char (&iter);
                }
                /* Blank line: stop — we've left the declaration block */
                if (gtk_text_iter_ends_line (&iter))
                    break;
                /* Stop if this line ends with a block/statement delimiter.
                 * A line ending in { } or ; is a complete statement or block
                 * opener/closer — not a continuation of the next line's header.
                 * Without this, scanning past an `if { }` block would find the
                 * outer scope's indentation and wrongly skip the else-if guide. */
                {
                    GtkTextIter tail = iter;
                    gunichar lc = 0;
                    gtk_text_iter_forward_to_line_end (&tail);
                    while (!gtk_text_iter_starts_line (&tail))
                    {
                        gtk_text_iter_backward_char (&tail);
                        lc = gtk_text_iter_get_char (&tail);
                        if (lc != ' ' && lc != '\t')
                            break;
                    }
                    if (lc == '{' || lc == '}' || lc == ';')
                        break;
                }
                /* Less-indented line found: this is the owner */
                if (scan_ws < best_ws)
                {
                    best_ws    = scan_ws;
                    guide_line = scan_line;
                    break;
                }
                /* Same or deeper indent: continuation line, keep going */
            }

            /* Get pixel x of first non-ws char on the owner line */
            gtk_text_buffer_get_iter_at_line (buffer, &iter, guide_line);
            while (!gtk_text_iter_ends_line (&iter))
            {
                ch = gtk_text_iter_get_char (&iter);
                if (ch != ' ' && ch != '\t') break;
                gtk_text_iter_forward_char (&iter);
            }
            gtk_text_view_get_iter_location (text_view, &iter, &iloc);
            guide_px = iloc.x;
        }

        /* Skip guides at x == 0: these are top-level blocks whose guide
         * would run along the leftmost edge of the text area. */
        if (guide_px <= 0)
            continue;

        /* Y coordinates: from line below { to the } line */
        gtk_text_buffer_get_iter_at_line (buffer, &iter, start_line);
        gtk_text_view_get_iter_location (text_view, &iter, &start_loc);

        gtk_text_buffer_get_iter_at_line (buffer, &iter, end_line);
        gtk_text_view_get_iter_location (text_view, &iter, &end_loc);

        {
            int tmp_x;
            gtk_text_view_buffer_to_window_coords (text_view,
                                                    GTK_TEXT_WINDOW_TEXT,
                                                    guide_px,
                                                    start_loc.y + start_loc.height,
                                                    &tmp_x, &y_top);
            win_x = tmp_x;
        }
        /* Use end_loc.y (not +height) so the guide stops at the top of the
         * closing `}` line rather than running all the way through it. */
        gtk_text_view_buffer_to_window_coords (text_view,
                                                GTK_TEXT_WINDOW_TEXT,
                                                0,
                                                end_loc.y,
                                                NULL, &y_bot);

        /* Draw the vertical guide */
        cairo_move_to (cr, win_x + 0.5, y_top);
        cairo_line_to (cr, win_x + 0.5, y_bot);
        cairo_stroke (cr);
    }

    cairo_restore (cr);
    g_slist_free (folds);
}

gpointer _moo_text_view_parent_class = NULL;

enum {
    PROP_0,
    PROP_BUFFER,

    PROP_INDENTER,
    PROP_AUTO_INDENT,
    PROP_BACKSPACE_INDENTS,
    PROP_TAB_INDENTS,

    PROP_RIGHT_MARGIN_OFFSET,
    PROP_DRAW_RIGHT_MARGIN,
    PROP_RIGHT_MARGIN_COLOR,
    PROP_HIGHLIGHT_CURRENT_LINE,
    PROP_HIGHLIGHT_CURRENT_LINE_UNFOCUSED,
    PROP_HIGHLIGHT_MATCHING_BRACKETS,
    PROP_HIGHLIGHT_MISMATCHING_BRACKETS,
    PROP_CURRENT_LINE_COLOR,
    PROP_TAB_WIDTH,
    PROP_DRAW_WHITESPACE,
    PROP_HAS_TEXT,
    PROP_HAS_SELECTION,
    PROP_CAN_UNDO,
    PROP_CAN_REDO,
    PROP_MANAGE_CLIPBOARD,
    PROP_SMART_HOME_END,
    PROP_ENABLE_HIGHLIGHT,
    PROP_SHOW_LINE_NUMBERS,
    PROP_SHOW_LINE_MARKS,
    PROP_ENABLE_FOLDING,
    PROP_ENABLE_QUICK_SEARCH,
    PROP_QUICK_SEARCH_FLAGS
};


/* MOO_TYPE_TEXT_VIEW */
G_DEFINE_TYPE_WITH_CODE (MooTextView, moo_text_view, GTK_TYPE_TEXT_VIEW,
                         G_IMPLEMENT_INTERFACE (MOO_TYPE_UNDO_OPS, undo_ops_iface_init))


static void moo_text_view_class_init (MooTextViewClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
    GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);
    GtkTextViewClass *text_view_class = GTK_TEXT_VIEW_CLASS (klass);
    GtkBindingSet *binding_set;

    _moo_text_view_parent_class = moo_text_view_parent_class;
    g_type_class_unref (g_type_class_ref (MOO_TYPE_INDENTER));
    /* TODO GTK3: g_type_class_add_private is deprecated.
       Consider using G_DEFINE_TYPE_WITH_PRIVATE instead. */
    g_type_class_add_private (klass, sizeof (MooTextViewPrivate));

    gobject_class->set_property = moo_text_view_set_property;
    gobject_class->get_property = moo_text_view_get_property;
    gobject_class->constructor = moo_text_view_constructor;
    gobject_class->finalize = moo_text_view_finalize;
    gobject_class->dispose = moo_text_view_dispose;

    widget_class->button_press_event = _moo_text_view_button_press_event;
    widget_class->button_release_event = _moo_text_view_button_release_event;
    widget_class->motion_notify_event = _moo_text_view_motion_event;

    widget_class->key_press_event = _moo_text_view_key_press_event;
    widget_class->realize = moo_text_view_realize;
    widget_class->scroll_event = moo_text_view_scroll_event;
    widget_class->unrealize = moo_text_view_unrealize;
    widget_class->draw = moo_text_view_expose;
    widget_class->style_set = moo_text_view_style_set;
    widget_class->get_preferred_width = moo_text_view_get_preferred_width;
    widget_class->get_preferred_height = moo_text_view_get_preferred_height;
    widget_class->size_allocate = moo_text_view_size_allocate;

    container_class->remove = moo_text_view_remove;

    text_view_class->move_cursor = _moo_text_view_move_cursor;
    text_view_class->delete_from_cursor = _moo_text_view_delete_from_cursor;
    text_view_class->copy_clipboard = moo_text_view_copy_clipboard;
    text_view_class->cut_clipboard = moo_text_view_cut_clipboard;
    text_view_class->paste_clipboard = moo_text_view_paste_clipboard;
    text_view_class->populate_popup = (void(*)(GtkTextView*,GtkWidget*)) moo_text_view_populate_popup;
    text_view_class->draw_layer = moo_text_view_draw_layer;

    klass->extend_selection = _moo_text_view_extend_selection;
    klass->find_word_at_cursor = find_word_at_cursor;
    klass->find_interactive = find_interactive;
    klass->find_next_interactive = find_next_interactive;
    klass->find_prev_interactive = find_prev_interactive;
    klass->replace_interactive = replace_interactive;
    klass->goto_line_interactive = goto_line_interactive;
    klass->char_inserted = moo_text_view_char_inserted;
    klass->apply_style_scheme = moo_text_view_apply_style_scheme;
    klass->get_text_cursor = moo_text_view_get_text_cursor;

    g_object_class_install_property (gobject_class,
                                     PROP_BUFFER,
                                     g_param_spec_object ("buffer",
                                             "buffer",
                                             "buffer",
                                             MOO_TYPE_TEXT_BUFFER,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY)));

    g_object_class_install_property (gobject_class,
                                     PROP_RIGHT_MARGIN_OFFSET,
                                     g_param_spec_uint ("right-margin-offset",
                                             "right-margin-offset",
                                             "right-margin-offset",
                                             1, G_MAXUINT, 80,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_DRAW_RIGHT_MARGIN,
                                     g_param_spec_boolean ("draw-right-margin",
                                             "draw-right-margin",
                                             "draw-right-margin",
                                             FALSE,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_RIGHT_MARGIN_COLOR,
                                     g_param_spec_string ("right-margin-color",
                                             "right-margin-color",
                                             "right-margin-color",
                                             LIGHT_BLUE,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_HIGHLIGHT_CURRENT_LINE,
                                     g_param_spec_boolean ("highlight-current-line",
                                             "highlight-current-line",
                                             "highlight-current-line",
                                             FALSE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_HIGHLIGHT_CURRENT_LINE_UNFOCUSED,
                                     g_param_spec_boolean ("highlight-current-line-unfocused",
                                             "highlight-current-line-unfocused",
                                             "highlight-current-line-unfocused",
                                             FALSE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_HIGHLIGHT_MATCHING_BRACKETS,
                                     g_param_spec_boolean ("highlight-matching-brackets",
                                             "highlight-matching-brackets",
                                             "highlight-matching-brackets",
                                             TRUE,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_HIGHLIGHT_MISMATCHING_BRACKETS,
                                     g_param_spec_boolean ("highlight-mismatching-brackets",
                                             "highlight-mismatching-brackets",
                                             "highlight-mismatching-brackets",
                                             FALSE,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_CURRENT_LINE_COLOR,
                                     g_param_spec_string ("current-line-color",
                                             "current-line-color",
                                             "current-line-color",
                                             LIGHT_BLUE,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_TAB_WIDTH,
                                     g_param_spec_uint ("tab-width",
                                             "tab-width",
                                             "tab-width",
                                             1, G_MAXUINT, 8,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_DRAW_WHITESPACE,
                                     g_param_spec_flags ("draw-whitespace",
                                             "draw-whitespace",
                                             "draw-whitespace",
                                             MOO_TYPE_DRAW_WS_FLAGS,
                                             0,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_INDENTER,
                                     g_param_spec_object ("indenter",
                                             "indenter",
                                             "indenter",
                                             MOO_TYPE_INDENTER,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_HAS_TEXT,
                                     g_param_spec_boolean ("has-text",
                                             "has-text",
                                             "has-text",
                                             FALSE,
                                             G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_HAS_SELECTION,
                                     g_param_spec_boolean ("has-selection",
                                             "has-selection",
                                             "has-selection",
                                             FALSE,
                                             G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_CAN_UNDO,
                                     g_param_spec_boolean ("can-undo",
                                             "can-undo",
                                             "can-undo",
                                             FALSE,
                                             G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_CAN_REDO,
                                     g_param_spec_boolean ("can-redo",
                                             "can-redo",
                                             "can-redo",
                                             FALSE,
                                             G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_MANAGE_CLIPBOARD,
                                     g_param_spec_boolean ("manage-clipboard",
                                             "manage-clipboard",
                                             "manage-clipboard",
                                             TRUE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_SMART_HOME_END,
                                     g_param_spec_boolean ("smart-home-end",
                                             "smart-home-end",
                                             "smart-home-end",
                                             TRUE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_ENABLE_HIGHLIGHT,
                                     g_param_spec_boolean ("enable-highlight",
                                             "enable-highlight",
                                             "enable-highlight",
                                             TRUE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_SHOW_LINE_NUMBERS,
                                     g_param_spec_boolean ("show-line-numbers",
                                             "show-line-numbers",
                                             "show-line-numbers",
                                             FALSE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_SHOW_LINE_MARKS,
                                     g_param_spec_boolean ("show-line-marks",
                                             "show-line-marks",
                                             "show-line-marks",
                                             FALSE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_ENABLE_FOLDING,
                                     g_param_spec_boolean ("enable-folding",
                                             "enable-folding",
                                             "enable-folding",
                                             FALSE,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     PROP_ENABLE_QUICK_SEARCH,
                                     g_param_spec_boolean ("enable-quick-search",
                                             "enable-quick-search",
                                             "enable-quick-search",
                                             TRUE,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_QUICK_SEARCH_FLAGS,
                                     g_param_spec_flags ("quick-search-flags",
                                             "quick-search-flags",
                                             "quick-search-flags",
                                             MOO_TYPE_TEXT_SEARCH_FLAGS,
                                             MOO_TEXT_SEARCH_CASELESS,
                                             (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_AUTO_INDENT,
                                     g_param_spec_boolean ("auto-indent",
                                             "auto-indent", "auto-indent",
                                             TRUE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_BACKSPACE_INDENTS,
                                     g_param_spec_boolean ("backspace-indents",
                                             "backspace-indents", "backspace-indents",
                                             FALSE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    g_object_class_install_property (gobject_class,
                                     PROP_TAB_INDENTS,
                                     g_param_spec_boolean ("tab-indents",
                                             "tab-indents", "tab-indents",
                                             FALSE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_CONSTRUCT)));

    gtk_widget_class_install_style_property (widget_class,
                                             g_param_spec_int ("expander-size",
                                                     "Expander Size",
                                                     "Size of the expander arrow",
                                                     0,
                                                     G_MAXINT,
                                                     DEFAULT_EXPANDER_SIZE,
                                                     G_PARAM_READABLE));

    signals[UNDO] =
            _moo_signal_new_cb ("undo",
                                G_OBJECT_CLASS_TYPE (klass),
                                G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                G_CALLBACK (moo_text_view_undo),
                                g_signal_accumulator_true_handled, NULL,
                                _moo_marshal_BOOLEAN__VOID,
                                G_TYPE_BOOLEAN, 0);

    signals[REDO] =
            _moo_signal_new_cb ("redo",
                                G_OBJECT_CLASS_TYPE (klass),
                                G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                G_CALLBACK (moo_text_view_redo),
                                g_signal_accumulator_true_handled, NULL,
                                _moo_marshal_BOOLEAN__VOID,
                                G_TYPE_BOOLEAN, 0);

    signals[DELETE_SELECTION] =
            _moo_signal_new_cb ("delete-selection",
                                G_OBJECT_CLASS_TYPE (klass),
                                G_SIGNAL_RUN_LAST,
                                G_CALLBACK (moo_text_view_delete_selection),
                                NULL, NULL,
                                _moo_marshal_VOID__VOID,
                                G_TYPE_NONE, 0);

    signals[FIND_WORD_AT_CURSOR] =
            g_signal_new ("find-word-at-cursor",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooTextViewClass, find_word_at_cursor),
                          NULL, NULL,
                          _moo_marshal_VOID__BOOLEAN,
                          G_TYPE_NONE, 1,
                          G_TYPE_BOOLEAN);

    signals[FIND_INTERACTIVE] =
            g_signal_new ("find-interactive",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooTextViewClass, find_interactive),
                          NULL, NULL,
                          _moo_marshal_VOID__VOID,
                          G_TYPE_NONE, 0);

    signals[FIND_NEXT_INTERACTIVE] =
            g_signal_new ("find-next-interactive",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooTextViewClass, find_next_interactive),
                          NULL, NULL,
                          _moo_marshal_VOID__VOID,
                          G_TYPE_NONE, 0);

    signals[FIND_PREV_INTERACTIVE] =
            g_signal_new ("find-prev-interactive",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooTextViewClass, find_prev_interactive),
                          NULL, NULL,
                          _moo_marshal_VOID__VOID,
                          G_TYPE_NONE, 0);

    signals[REPLACE_INTERACTIVE] =
            g_signal_new ("replace-interactive",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooTextViewClass, replace_interactive),
                          NULL, NULL,
                          _moo_marshal_VOID__VOID,
                          G_TYPE_NONE, 0);

    signals[GOTO_LINE_INTERACTIVE] =
            g_signal_new ("goto-line-interactive",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooTextViewClass, goto_line_interactive),
                          NULL, NULL,
                          _moo_marshal_VOID__VOID,
                          G_TYPE_NONE, 0);

    signals[CHAR_INSERTED] =
            g_signal_new ("char-inserted",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST,
                          G_STRUCT_OFFSET (MooTextViewClass, char_inserted),
                          g_signal_accumulator_true_handled, NULL,
                          _moo_marshal_BOOLEAN__BOXED_STRING,
                          G_TYPE_BOOLEAN, 2,
                          GTK_TYPE_TEXT_ITER | G_SIGNAL_TYPE_STATIC_SCOPE,
                          G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

    signals[CURSOR_MOVED] =
            g_signal_new ("cursor-moved",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST,
                          0, NULL, NULL,
                          _moo_marshal_VOID__BOXED,
                          G_TYPE_NONE, 1,
                          GTK_TYPE_TEXT_ITER | G_SIGNAL_TYPE_STATIC_SCOPE);

    signals[LINE_MARK_CLICKED] =
            g_signal_new ("line-mark-clicked",
                          G_OBJECT_CLASS_TYPE (klass),
                          G_SIGNAL_RUN_LAST,
                          G_STRUCT_OFFSET (MooTextViewClass, line_mark_clicked),
                          g_signal_accumulator_true_handled, NULL,
                          _moo_marshal_BOOLEAN__INT,
                          G_TYPE_BOOLEAN, 1,
                          G_TYPE_INT);

    signals[START_QUICK_SEARCH] =
            _moo_signal_new_cb ("start-quick-search",
                                G_OBJECT_CLASS_TYPE (klass),
                                G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                G_CALLBACK (start_quick_search),
                                NULL, NULL,
                                _moo_marshal_BOOLEAN__VOID,
                                G_TYPE_BOOLEAN, 0);

    binding_set = gtk_binding_set_by_class (klass);
    gtk_binding_entry_add_signal (binding_set, GDK_KEY_z, MOO_ACCEL_CTRL_MASK,
                                  "undo", 0);
    gtk_binding_entry_add_signal (binding_set, GDK_KEY_z, MOO_ACCEL_CTRL_MASK | GDK_SHIFT_MASK,
                                  "redo", 0);
}


static void
moo_text_view_init (MooTextView *view)
{
    view->priv = G_TYPE_INSTANCE_GET_PRIVATE (view, MOO_TYPE_TEXT_VIEW, MooTextViewPrivate);

    view->priv->constructed = FALSE;
    view->priv->buffer_type = MOO_TYPE_TEXT_BUFFER;
    view->priv->buffer = NULL;

    view->priv->colors[MOO_TEXT_VIEW_COLOR_CURRENT_LINE] = g_strdup (LIGHT_BLUE);
    view->priv->colors[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN] = g_strdup (LIGHT_BLUE);
    view->priv->right_margin_offset = 80;

    view->priv->word_chars = NULL;
    view->priv->n_word_chars = 0;

    view->priv->dnd.button = GDK_BUTTON_RELEASE;
    view->priv->dnd.type = MOO_TEXT_VIEW_DRAG_NONE;
    view->priv->dnd.start_x = -1;
    view->priv->dnd.start_y = -1;

    view->priv->last_search_stamp = -1;

    view->priv->tab_indents = FALSE;
    view->priv->backspace_indents = FALSE;
    view->priv->enter_indents = TRUE;
    view->priv->ctrl_up_down_scrolls = TRUE;
    view->priv->ctrl_page_up_down_scrolls = TRUE;
    view->priv->smart_home_end = TRUE;

    view->priv->highlight_matching_brackets = TRUE;
    view->priv->highlight_mismatching_brackets = FALSE;
    view->priv->tab_width = 8;

    view->priv->qs.flags = MOO_TEXT_SEARCH_CASELESS;

    view->priv->lm.show_icons = FALSE;
    view->priv->lm.show_numbers = FALSE;
    view->priv->lm.numbers_font = NULL;
    view->priv->lm.show_folds = FALSE;
    view->priv->n_lines = 1;

    gtk_text_view_set_left_margin (GTK_TEXT_VIEW (view), 2);
    gtk_text_view_set_right_margin (GTK_TEXT_VIEW (view), 2);
}


void
moo_text_view_set_buffer_type (MooTextView *view,
                               GType        type)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (g_type_is_a (type, MOO_TYPE_TEXT_BUFFER));

    if (!g_type_is_a (view->priv->buffer_type, type))
        view->priv->buffer_type = type;
}

/* ------------------------------------------------------------------ */
/* Automatic brace-based code folding                                  */
/* ------------------------------------------------------------------ */

static void
moo_fold_clear_all (MooTextBuffer *mbuf)
{
    /* Remove the invisible tag from the entire buffer in one shot, then
     * discard every fold (including stale folds whose line marks were
     * already deleted because the line was removed before the rescan timer
     * fired).  moo_text_buffer_clear_all_folds() traverses the fold tree
     * directly so it is not confused by marks with priv->line == NULL. */
    GtkTextIter ts, te;

    gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (mbuf), &ts, &te);
    gtk_text_buffer_remove_tag_by_name (GTK_TEXT_BUFFER (mbuf), MOO_FOLD_TAG, &ts, &te);

    moo_text_buffer_clear_all_folds (mbuf);
}

/* Files larger than this line count skip fold scanning to keep editing
 * responsive.  The full-file text copy + walk done by moo_fold_scan_braces
 * is O(n) and fires every 400 ms after each edit, which makes typing in a
 * 25 k-line YAML (or any large) file unbearably slow. */
#define MOO_FOLD_LINE_LIMIT 5000

static void
/* FAST_FOLD_SCAN — replacement using raw text scanning */
moo_fold_scan_braces (GtkTextView *text_view)
{
    MooTextView *view;
    GtkTextBuffer *buffer;
    MooTextBuffer *mbuf;
    int line_count;
    int *stack;
    int stack_size = 0, stack_cap = 256;
    gboolean in_string = FALSE, in_char = FALSE;
    gboolean in_line_comment = FALSE, in_block_comment = FALSE;
    char prev_ch = 0;
    char *text;
    const char *p;
    int line;
    gboolean is_end_lang   = FALSE;  /* Matlab/Octave: keyword...end blocks */
    gboolean is_pascal     = FALSE;  /* Pascal: begin...end blocks */
    gboolean is_indent_lang = FALSE; /* Python/YAML: indentation-based blocks */

    buffer = gtk_text_view_get_buffer (text_view);
    if (!MOO_IS_TEXT_BUFFER (buffer))
        return;
    mbuf = MOO_TEXT_BUFFER (buffer);
    if (!mbuf)
        return;
    view = MOO_TEXT_VIEW (text_view);

    /* Block all fold-signal handlers on the view to prevent a cascade of
     * per-fold redraws (hundreds of gtk_widget_queue_draw calls) during the
     * clear + rebuild cycle.  A single queue_draw at the end is enough. */
    g_signal_handlers_block_by_func (buffer, (gpointer) fold_added,   view);
    g_signal_handlers_block_by_func (buffer, (gpointer) fold_deleted, view);
    g_signal_handlers_block_by_func (buffer, (gpointer) fold_toggled, view);
    g_signal_handlers_block_by_func (buffer, (gpointer) _moo_text_view_on_fold_toggled, view);

    /* Save collapsed fold lines using fold range query (not per-line) */
    line_count = gtk_text_buffer_get_line_count (buffer);
    {
        GArray *collapsed_lines = g_array_new (FALSE, FALSE, sizeof(int));
        if (line_count > 1)
        {
            GSList *all_folds = moo_text_buffer_get_folds_in_range (mbuf, 0, line_count - 1);
            GSList *fl;
            for (fl = all_folds; fl; fl = fl->next)
            {
                MooFold *_f = (MooFold *) fl->data;
                if (_f && !_moo_fold_is_deleted (_f) && _f->collapsed)
                {
                    int cline = _moo_fold_get_start (_f);
                    if (cline >= 0)  /* skip stale folds with deleted marks */
                        g_array_append_val (collapsed_lines, cline);
                }
            }
            g_slist_free (all_folds);
        }
        moo_fold_clear_all (mbuf);
        g_object_set_data_full (G_OBJECT (buffer), "moo-fold-collapsed-lines",
                                collapsed_lines, (GDestroyNotify) g_array_unref);
    }

    if (line_count < 2)
    {
        g_signal_handlers_unblock_by_func (buffer, (gpointer) _moo_text_view_on_fold_toggled, view);
        g_signal_handlers_unblock_by_func (buffer, (gpointer) fold_added,   view);
        g_signal_handlers_unblock_by_func (buffer, (gpointer) fold_deleted, view);
        g_signal_handlers_unblock_by_func (buffer, (gpointer) fold_toggled, view);
        return;
    }

    /* Get entire buffer as raw C string — orders of magnitude faster
     * than per-character GtkTextIter walking */
    {
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds (buffer, &start, &end);
        /* Use TRUE to include "invisible" (long-line-truncated) characters so
         * that braces hidden by the rendering optimisation are still counted. */
        text = gtk_text_buffer_get_text (buffer, &start, &end, TRUE);
    }

    stack = (int *) g_malloc (stack_cap * sizeof (int));
    line = 0;

    /* Detect language to choose folding strategy */
    {
        MooLang   *lang    = moo_text_buffer_get_lang (mbuf);
        const char *lang_id = _moo_lang_id (lang);
        is_end_lang   = (strcmp (lang_id, "matlab") == 0 ||
                         strcmp (lang_id, "octave") == 0);
        is_pascal     =  strcmp (lang_id, "pascal") == 0;
        is_indent_lang = (strcmp (lang_id, "python")  == 0 ||
                          strcmp (lang_id, "python3") == 0 ||
                          strcmp (lang_id, "yaml")    == 0);
    }

    if (is_indent_lang)
    {
        /* ── Indentation-based fold scanner (Python, YAML) ───────────
         * A fold opens when a non-blank/non-comment line is followed by
         * a line with strictly greater indentation; the opener is the
         * line BEFORE the indent increase.  A fold closes when
         * indentation drops back below the level at which it opened.
         * Blank lines and comment-only lines are skipped so they do not
         * interrupt a block.
         *
         * Two parallel arrays track open blocks:
         *   stack[i]  — start line of block i
         *   istack[i] — indent level that opened block i
         */
        int *istack = (int *) g_malloc (stack_cap * sizeof (int));
        int prev_indent    = -1;
        int prev_nonempty  = -1;
        const char *ls = text;

        while (*ls)
        {
            /* Find end of line */
            const char *le = ls;
            while (*le && *le != '\n' && *le != '\r')
                le++;

            /* Measure indentation and detect blank/comment lines */
            const char *pw = ls;
            int indent = 0;
            while (pw < le && (*pw == ' ' || *pw == '\t'))
            {
                indent++;
                pw++;
            }
            gboolean blank   = (pw == le);
            gboolean comment = (!blank && *pw == '#');  /* # for Python/YAML */

            if (!blank && !comment)
            {
                if (prev_indent >= 0)
                {
                    if (indent > prev_indent)
                    {
                        /* Indentation increased: prev_nonempty line opens a block */
                        if (stack_size >= stack_cap)
                        {
                            stack_cap *= 2;
                            stack  = (int *) g_realloc (stack,  stack_cap * sizeof (int));
                            istack = (int *) g_realloc (istack, stack_cap * sizeof (int));
                        }
                        istack[stack_size] = indent;
                        stack [stack_size] = prev_nonempty;
                        stack_size++;
                    }
                    else if (indent < prev_indent)
                    {
                        /* Dedent: close all blocks opened at a deeper level.
                         * Use the current line (dedent trigger) as fold_end so
                         * that _moo_fold_tree_collapse (which hides start+1 ..
                         * end_line-1) covers through prev_nonempty. */
                        while (stack_size > 0 && istack[stack_size - 1] > indent)
                        {
                            stack_size--;
                            int open_line = stack[stack_size];
                            if (line > open_line)
                                moo_text_buffer_add_fold (mbuf, open_line, line);
                        }
                    }
                }
                prev_indent   = indent;
                prev_nonempty = line;
            }

            /* Advance past line ending */
            if (*le == '\r' && *(le + 1) == '\n')
                ls = le + 2;
            else if (*le == '\n' || *le == '\r')
                ls = le + 1;
            else
                break;
            line++;
        }

        /* End of buffer: close any still-open blocks.
         * Use prev_nonempty+1 as fold_end so collapse hides through
         * prev_nonempty (collapse hides start+1 .. end_line-1). */
        {
            int line_count = gtk_text_buffer_get_line_count ((GtkTextBuffer *) mbuf);
            int eof_end = (prev_nonempty + 1 < line_count) ? prev_nonempty + 1 : prev_nonempty;
            while (stack_size > 0)
            {
                stack_size--;
                int open_line = stack[stack_size];
                if (eof_end > open_line)
                    moo_text_buffer_add_fold (mbuf, open_line, eof_end);
            }
        }

        g_free (istack);
    }
    else if (is_end_lang || is_pascal)
    {
        /* ── Line-by-line keyword/end block scanner ──────────────────
         * Matlab/Octave: block openers are keywords like function/if/for/…
         * followed by a matching "end" (or endfor/endif/… in Octave).
         * Pascal: "begin"/"record"/"object"/"try" matched with "end".
         * Only the first word on a line (after whitespace) is examined;
         * this avoids misidentifying "end" inside index expressions like
         * A(1:end) which never appears as the first token of a statement.
         */
        static const char *matlab_openers[] = {
            "function", "if", "for", "while", "switch",
            "try", "classdef", "methods", "properties",
            "events", "enumeration", "parfor", NULL
        };
        static const char *octave_end_compounds[] = {
            "if", "for", "while", "function", "switch",
            "try", "classdef", "methods", "properties",
            "events", "enumeration", NULL
        };
        static const char *pascal_openers[] = {
            "begin", "record", "object", "try", NULL
        };

        const char *ls = text;   /* line start */
        while (*ls)
        {
            /* Find end of line */
            const char *le = ls;
            while (*le && *le != '\n' && *le != '\r')
                le++;

            /* Skip leading whitespace */
            const char *w = ls;
            while (w < le && (*w == ' ' || *w == '\t'))
                w++;

            if (w < le)
            {
                gboolean is_opener = FALSE;
                gboolean is_closer = FALSE;
                size_t remaining = (size_t)(le - w);

                if (is_end_lang)
                {
                    /* Skip comment lines */
                    if (*w != '%' && *w != '#')
                    {
                        int ki;
                        for (ki = 0; matlab_openers[ki] && !is_opener; ki++)
                        {
                            size_t klen = strlen (matlab_openers[ki]);
                            if (remaining >= klen &&
                                strncmp (w, matlab_openers[ki], klen) == 0)
                            {
                                char nx = w[klen];
                                if (nx == ' ' || nx == '\t' || nx == '(' ||
                                    nx == '%' || nx == '#'  || nx == '\0' ||
                                    nx == '\n' || nx == '\r')
                                    is_opener = TRUE;
                            }
                        }
                        if (!is_opener && remaining >= 3 &&
                            strncmp (w, "end", 3) == 0)
                        {
                            char nx = w[3];
                            if (nx == '\0' || nx == ' '  || nx == '\t' ||
                                nx == ';'  || nx == '%'  || nx == '#'  ||
                                nx == '\n' || nx == '\r')
                            {
                                is_closer = TRUE;
                            }
                            else
                            {
                                /* Octave compound: endfor, endif, endwhile … */
                                int ki;
                                for (ki = 0; octave_end_compounds[ki] && !is_closer; ki++)
                                {
                                    size_t klen = strlen (octave_end_compounds[ki]);
                                    if (remaining >= 3 + klen &&
                                        strncmp (w + 3, octave_end_compounds[ki], klen) == 0)
                                    {
                                        char after = w[3 + klen];
                                        if (after == '\0' || after == ' '  ||
                                            after == '\t'  || after == ';' ||
                                            after == '%'   || after == '#' ||
                                            after == '\n'  || after == '\r')
                                            is_closer = TRUE;
                                    }
                                }
                            }
                        }
                    }
                }
                else /* is_pascal */
                {
                    /* Skip // line comments */
                    if (!(w[0] == '/' && w[1] == '/'))
                    {
                        int ki;
                        for (ki = 0; pascal_openers[ki] && !is_opener; ki++)
                        {
                            size_t klen = strlen (pascal_openers[ki]);
                            if (remaining >= klen &&
                                g_ascii_strncasecmp (w, pascal_openers[ki], klen) == 0)
                            {
                                char nx = w[klen];
                                if (nx == '\0' || nx == ' '  || nx == '\t' ||
                                    nx == ';'  || nx == '/'  ||
                                    nx == '\n' || nx == '\r')
                                    is_opener = TRUE;
                            }
                        }
                        if (!is_opener && remaining >= 3 &&
                            g_ascii_strncasecmp (w, "end", 3) == 0)
                        {
                            char nx = w[3];
                            if (nx == '\0' || nx == ' '  || nx == '\t' ||
                                nx == ';'  || nx == '.'  || nx == '/'  ||
                                nx == '\n' || nx == '\r')
                                is_closer = TRUE;
                        }
                    }
                }

                if (is_opener)
                {
                    if (stack_size >= stack_cap)
                    {
                        stack_cap *= 2;
                        stack = (int *) g_realloc (stack, stack_cap * sizeof (int));
                    }
                    stack[stack_size++] = line;
                }
                else if (is_closer && stack_size > 0)
                {
                    int open_line = stack[--stack_size];
                    if (line > open_line)
                        moo_text_buffer_add_fold (mbuf, open_line, line);
                }
            }

            /* Advance past line ending */
            if (*le == '\r' && *(le + 1) == '\n')
                ls = le + 2;
            else if (*le == '\n' || *le == '\r')
                ls = le + 1;
            else
                break;
            line++;
        }
    }
    else
    {
        /* ── Character-by-character brace scanner ({/}) ────────────── */
        for (p = text; *p; p++)
        {
            char ch = *p;

            if (ch == '\n')
            {
                line++;
                in_line_comment = FALSE;
                prev_ch = ch;
                continue;
            }
            if (ch == '\r')
            {
                if (*(p + 1) == '\n') p++;
                line++;
                in_line_comment = FALSE;
                prev_ch = '\n';
                continue;
            }

            if (in_block_comment)
            {
                if (ch == '/' && prev_ch == '*')
                    in_block_comment = FALSE;
            }
            else if (in_line_comment)
            {
                /* skip until newline */
            }
            else if (in_string)
            {
                if (ch == '"' && prev_ch != '\\')
                    in_string = FALSE;
                else if (ch == '\\' && prev_ch == '\\')
                {
                    prev_ch = 0;
                    continue;
                }
            }
            else if (in_char)
            {
                if (ch == '\'' && prev_ch != '\\')
                    in_char = FALSE;
                else if (ch == '\\' && prev_ch == '\\')
                {
                    prev_ch = 0;
                    continue;
                }
            }
            else
            {
                if (ch == '"')
                    in_string = TRUE;
                else if (ch == '\'')
                    in_char = TRUE;
                else if (ch == '/' && *(p + 1) == '/')
                    in_line_comment = TRUE;
                else if (ch == '/' && *(p + 1) == '*')
                    in_block_comment = TRUE;
                else if (ch == '{')
                {
                    if (stack_size >= stack_cap)
                    {
                        stack_cap *= 2;
                        stack = (int *) g_realloc (stack, stack_cap * sizeof (int));
                    }
                    stack[stack_size++] = line;
                }
                else if (ch == '}' && stack_size > 0)
                {
                    int open_line = stack[--stack_size];
                    if (line > open_line)
                        moo_text_buffer_add_fold (mbuf, open_line, line);
                }
            }

            prev_ch = ch;
        }
    } /* end brace scanner */

    g_free (stack);
    g_free (text);

    /* Restore collapsed state.
     * Unblock _moo_text_view_on_fold_toggled first so it correctly applies
     * the fold-header highlight tag and manages long-line markers for each
     * fold being collapsed.  The other redraw handlers stay blocked. */
    g_signal_handlers_unblock_by_func (buffer, (gpointer) _moo_text_view_on_fold_toggled, view);
    {
        GArray *collapsed_lines = (GArray *) g_object_get_data (
            G_OBJECT (buffer), "moo-fold-collapsed-lines");
        if (collapsed_lines)
        {
            guint _i;
            for (_i = 0; _i < collapsed_lines->len; _i++)
            {
                int cline = g_array_index (collapsed_lines, int, _i);
                MooFold *_f = moo_text_buffer_get_fold_at_line (mbuf, cline);
                if (_f && !_f->collapsed)
                    moo_text_buffer_toggle_fold (mbuf, _f);
            }
            g_object_set_data (G_OBJECT (buffer), "moo-fold-collapsed-lines", NULL);
        }
    }

    /* Unblock the remaining redraw handlers and do a single full repaint */
    g_signal_handlers_unblock_by_func (buffer, (gpointer) fold_added,   view);
    g_signal_handlers_unblock_by_func (buffer, (gpointer) fold_deleted, view);
    g_signal_handlers_unblock_by_func (buffer, (gpointer) fold_toggled, view);
    gtk_widget_queue_draw (GTK_WIDGET (view));
}

/* Debounced fold update: simple reset-timer debounce.
 * Each buffer change resets a single timer; the rescan only fires
 * after 400 ms of inactivity, preventing rapid-typing rescans. */

static gboolean
moo_fold_timer_cb (gpointer data)
{
    MooTextView *view = MOO_TEXT_VIEW (data);
    guint *timer_id = (guint *) g_object_get_data (G_OBJECT (view), "moo-fold-timer-id");
    if (timer_id) *timer_id = 0;
    if (view->priv->enable_folding)
    {
        /* Skip edit-triggered rescans for large files — the initial scan at
         * file-open still runs once (to create fold markers), but repeated
         * O(n) rescans while typing would freeze the UI on large files. */
        GtkTextBuffer *_buf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
        if (gtk_text_buffer_get_line_count (_buf) <= MOO_FOLD_LINE_LIMIT)
            moo_fold_scan_braces (GTK_TEXT_VIEW (view));
    }
    return G_SOURCE_REMOVE;
}

static void
fold_timer_id_free (guint *timer_id)
{
    /* Cancel the pending timeout before freeing so the callback never
     * fires on a destroyed view (use-after-free / segfault). */
    if (timer_id && *timer_id != 0)
        g_source_remove (*timer_id);
    g_free (timer_id);
}

static void
moo_fold_schedule_update (MooTextView *view)
{
    guint *timer_id;

    if (!view->priv->enable_folding)
        return;

    timer_id = (guint *) g_object_get_data (G_OBJECT (view), "moo-fold-timer-id");
    if (!timer_id)
    {
        timer_id = g_new0 (guint, 1);
        g_object_set_data_full (G_OBJECT (view), "moo-fold-timer-id",
                                timer_id, (GDestroyNotify) fold_timer_id_free);
    }

    /* Cancel the old timer and restart — rescan only after 400 ms of no edits */
    if (*timer_id != 0)
        g_source_remove (*timer_id);
    *timer_id = g_timeout_add (400, moo_fold_timer_cb, view);
}

/* Called after buffer text changes */
static void
moo_fold_after_buffer_changed (MooTextView *view)
{
    /* Don't bother scheduling a rescan for large files — moo_fold_scan_braces
     * would bail out immediately anyway, but this avoids the repeated
     * g_source_remove / g_timeout_add churn on every single keystroke. */
    if (gtk_text_buffer_get_line_count (
            gtk_text_view_get_buffer (GTK_TEXT_VIEW (view))) > MOO_FOLD_LINE_LIMIT)
        return;

    moo_fold_schedule_update (view);
}

/* Called when the buffer's language property changes (e.g. after deferred
 * language detection at file-open).  We always run a fresh fold scan so
 * that YAML/Python indent folds (which require knowing the language) are
 * created even for large files.  This runs at most once per language change,
 * not on every edit, so the one-time cost is acceptable. */
static void
moo_fold_on_lang_changed (MooTextView *view)
{
    if (view->priv->enable_folding)
        moo_fold_scan_braces (GTK_TEXT_VIEW (view));
}


static void
connect_buffer (MooTextView *view)
{


    MooUndoStack *undo_stack;
    GtkTextBuffer *buffer = view->priv->buffer;

    g_return_if_fail (buffer != NULL);

    g_signal_connect_swapped (buffer, "changed",
                              G_CALLBACK (buffer_changed), view);
    g_signal_connect_swapped (buffer, "cursor_moved",
                              G_CALLBACK (cursor_moved), view);
    g_signal_connect_swapped (buffer, "selection-changed",
                              G_CALLBACK (selection_changed), view);
    g_signal_connect_swapped (buffer, "highlight-updated",
                              G_CALLBACK (highlight_updated), view);
    g_signal_connect_swapped (buffer, "notify::has-selection",
                              G_CALLBACK (proxy_prop_notify), view);
    g_signal_connect_swapped (buffer, "notify::has-text",
                              G_CALLBACK (proxy_prop_notify), view);

    undo_stack = _moo_text_buffer_get_undo_stack (MOO_TEXT_BUFFER (buffer));
    g_signal_connect_swapped (undo_stack, "notify::can-undo",
                              G_CALLBACK (proxy_notify_can_undo_redo), view);
    g_signal_connect_swapped (undo_stack, "notify::can-redo",
                              G_CALLBACK (proxy_notify_can_undo_redo), view);

    g_signal_connect_data (buffer, "insert-text",
                           G_CALLBACK (insert_text_cb), view,
                           NULL, G_CONNECT_AFTER | G_CONNECT_SWAPPED);

    /* Long-line click-to-reveal and wrap-mode tracking */
    g_signal_connect (view, "button-press-event",
                      G_CALLBACK (moo_ll_click_handler), NULL);
    g_signal_connect (view, "notify::wrap-mode",
                      G_CALLBACK (moo_ll_wrap_changed), NULL);
    g_signal_connect_after (buffer, "insert-text",
                            G_CALLBACK (moo_ll_after_insert), NULL);
    {
        GtkWrapMode _wm = gtk_text_view_get_wrap_mode (GTK_TEXT_VIEW (view));
        g_object_set_data (G_OBJECT (buffer), "moo-nowrap-mode",
                           GINT_TO_POINTER (_wm == GTK_WRAP_NONE ? 1 : 0));
    }
    moo_ll_apply (buffer);

    /* Enable folding and scan for brace folds */
    if (!view->priv->enable_folding)
        set_enable_folding (view, TRUE);
    moo_fold_scan_braces (GTK_TEXT_VIEW (view));
    g_signal_connect_swapped (buffer, "changed",
                              G_CALLBACK (moo_fold_after_buffer_changed), view);
    /* Re-scan folds when language changes (e.g. after deferred language detection).
     * Uses moo_fold_on_lang_changed (not the debounced timer path) so that
     * large YAML/Python files still get their indent folds on open. */
    g_signal_connect_swapped (buffer, "notify::lang",
                              G_CALLBACK (moo_fold_on_lang_changed), view);


    g_signal_connect_swapped (buffer, "line-mark-added",
                              G_CALLBACK (line_mark_added), view);
    g_signal_connect_swapped (buffer, "line-mark-moved",
                              G_CALLBACK (line_mark_moved), view);
    g_signal_connect_swapped (buffer, "line-mark-deleted",
                              G_CALLBACK (line_mark_deleted), view);
    g_signal_connect_swapped (buffer, "fold-added",
                              G_CALLBACK (fold_added), view);
    g_signal_connect_swapped (buffer, "fold-deleted",
                              G_CALLBACK (fold_deleted), view);
    g_signal_connect_swapped (buffer, "fold-toggled",
                              G_CALLBACK (fold_toggled), view);
}

static void
disconnect_buffer (MooTextView *view)
{
    MooUndoStack *undo_stack;

    /* Disconnect long-line handlers */
    g_signal_handlers_disconnect_by_func (GTK_WIDGET (view),
                                          (gpointer) moo_ll_click_handler, NULL);
    g_signal_handlers_disconnect_by_func (GTK_WIDGET (view),
                                          (gpointer) moo_ll_wrap_changed, NULL);
    if (!view->priv->buffer)
        return;

    /* Disconnect long-line after-insert handler (connected with NULL data) */
    g_signal_handlers_disconnect_by_func (view->priv->buffer,
                                          (gpointer) moo_ll_after_insert, NULL);

    /* Disconnect fold update handler and cancel any pending rescan timer */
    g_signal_handlers_disconnect_by_func (view->priv->buffer,
                                          (gpointer) moo_fold_after_buffer_changed, view);
    {
        guint *timer_id = (guint *) g_object_get_data (G_OBJECT (view), "moo-fold-timer-id");
        if (timer_id && *timer_id)
        {
            g_source_remove (*timer_id);
            *timer_id = 0;
        }
    }
    g_signal_handlers_disconnect_matched (view->priv->buffer,
                                          G_SIGNAL_MATCH_DATA,
                                          0, 0, NULL, NULL,
                                          view);

    undo_stack = _moo_text_buffer_get_undo_stack (MOO_TEXT_BUFFER (view->priv->buffer));

    g_signal_handlers_disconnect_matched (undo_stack,
                                          G_SIGNAL_MATCH_DATA,
                                          0, 0, NULL, NULL,
                                          view);
}

static GObject*
moo_text_view_constructor (GType                  type,
                           guint                  n_construct_properties,
                           GObjectConstructParam *construct_param)
{
    GtkTargetList *target_list;
    GObject *object;
    MooTextView *view;

    object = G_OBJECT_CLASS (moo_text_view_parent_class)->constructor (
        type, n_construct_properties, construct_param);

    view = MOO_TEXT_VIEW (object);

    if (!view->priv->buffer)
    {
        view->priv->buffer = GTK_TEXT_BUFFER (g_object_new (view->priv->buffer_type, (const char*) NULL));
        gtk_text_view_set_buffer (GTK_TEXT_VIEW (view), view->priv->buffer);
    }

    g_assert (view->priv->buffer == gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)));

    view->priv->constructed = TRUE;

    _moo_text_view_init_fold_visuals (view);

    g_object_set (view->priv->buffer,
                  "highlight-matching-brackets", view->priv->highlight_matching_brackets,
                  "highlight-mismatching-brackets", view->priv->highlight_mismatching_brackets,
                  NULL);

    connect_buffer (view);

    target_list = gtk_target_list_new (text_view_target_table, G_N_ELEMENTS (text_view_target_table));
    gtk_target_list_add_text_targets (target_list, 0);
    gtk_drag_dest_set_target_list (GTK_WIDGET (view), target_list);
    gtk_target_list_unref (target_list);

    return object;
}


static void
moo_text_view_dispose (GObject *object)
{
    MooTextView *view = MOO_TEXT_VIEW (object);

    disconnect_buffer (view);

    if (view->priv->buffer)
    {
        g_object_unref (view->priv->buffer);
        view->priv->buffer = NULL;
    }

    if (view->priv->clipboard)
    {
        view->priv->clipboard->view = NULL;
        view->priv->clipboard = NULL;
    }

    if (view->priv->indenter)
    {
        g_object_unref (view->priv->indenter);
        view->priv->indenter = NULL;
    }

    while (view->priv->line_marks)
        remove_line_mark (view, view->priv->line_marks->data);

    if (view->priv->style_scheme)
    {
        g_object_unref (view->priv->style_scheme);
        view->priv->style_scheme = NULL;
    }

    if (view->priv->move_cursor_idle)
    {
        g_source_remove (view->priv->move_cursor_idle);
        view->priv->move_cursor_idle = 0;
    }

    G_OBJECT_CLASS (moo_text_view_parent_class)->dispose (object);
}

static void
moo_text_view_finalize (GObject *object)
{
    int i;
    MooTextView *view = MOO_TEXT_VIEW (object);

    for (i = 0; i < MOO_TEXT_VIEW_N_COLORS; ++i)
        g_free (view->priv->colors[i]);

    g_free (view->priv->char_inserted);
    /* box_sel_clear() deliberately keeps this across selections, so nothing
       else ever frees it. */
    g_free (view->priv->box_sel.copied_text);
    view->priv->box_sel.copied_text = NULL;

    G_OBJECT_CLASS (moo_text_view_parent_class)->finalize (object);
}


static void
moo_text_view_delete_selection (MooTextView *view)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    gtk_text_buffer_delete_selection (get_buffer (view), TRUE, TRUE);
}


static void
moo_text_view_message (MooTextView *view,
                       const char  *message)
{
    GtkWidget *toplevel;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (message != NULL);

    toplevel = gtk_widget_get_toplevel (GTK_WIDGET (view));

    if (MOO_IS_EDIT_WINDOW (toplevel))
        moo_window_message (MOO_WINDOW (toplevel), message);
}


static void
msg_to_statusbar (const char *message,
                  gpointer    data)
{
    moo_text_view_message (data, message);
}


static void
find_word_at_cursor (MooTextView *view,
                     gboolean     forward)
{
    moo_text_view_run_find_current_word (GTK_TEXT_VIEW (view), forward,
                                         msg_to_statusbar, view);
}

static void
find_interactive (MooTextView *view)
{
    moo_text_view_run_find (GTK_TEXT_VIEW (view),
                            msg_to_statusbar, view);
}

static void
replace_interactive (MooTextView *view)
{
    moo_text_view_run_replace (GTK_TEXT_VIEW (view),
                               msg_to_statusbar, view);
}

static void
find_next_interactive (MooTextView *view)
{
    moo_text_view_run_find_next (GTK_TEXT_VIEW (view),
                                 msg_to_statusbar, view);
}

static void
find_prev_interactive (MooTextView *view)
{
    moo_text_view_run_find_prev (GTK_TEXT_VIEW (view),
                                 msg_to_statusbar, view);
}

static void
goto_line_interactive (MooTextView *view)
{
    moo_text_view_run_goto_line (GTK_TEXT_VIEW (view));
}


/**
 * moo_text_view_set_font_from_string: (moo.private 1)
 *
 * @view:
 * @font: (type const-utf8)
 **/
void
moo_text_view_set_font_from_string (MooTextView *view,
                                    const char  *font)
{
    PangoFontDescription *font_desc = NULL;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (font)
        font_desc = pango_font_description_from_string (font);

    gtk_widget_override_font (GTK_WIDGET (view), font_desc);

    if (font_desc)
        pango_font_description_free (font_desc);
}


static MooUndoStack *
get_undo_stack (MooTextView *view)
{
    return _moo_text_buffer_get_undo_stack (get_moo_buffer (view));
}


gboolean
moo_text_view_can_redo (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), FALSE);
    return moo_undo_stack_can_redo (get_undo_stack (view));
}


gboolean
moo_text_view_can_undo (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), FALSE);
    return moo_undo_stack_can_undo (get_undo_stack (view));
}


gboolean
moo_text_view_redo (MooTextView    *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), FALSE);

    if (moo_undo_stack_can_redo (get_undo_stack (view)) &&
        gtk_text_view_get_editable (GTK_TEXT_VIEW (view)))
    {
        moo_text_buffer_freeze (get_moo_buffer (view));
        moo_undo_stack_redo (get_undo_stack (view));
        gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (view),
                                      get_insert (view),
                                      0, FALSE, 0, 0);
        moo_text_buffer_thaw (get_moo_buffer (view));
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}


gboolean
moo_text_view_undo (MooTextView    *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), FALSE);

    if (moo_undo_stack_can_undo (get_undo_stack (view)) &&
        gtk_text_view_get_editable (GTK_TEXT_VIEW (view)))
    {
        moo_text_buffer_freeze (get_moo_buffer (view));
        moo_undo_stack_undo (get_undo_stack (view));
        gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (view),
                                      get_insert (view),
                                      0, FALSE, 0, 0);
        moo_text_buffer_thaw (get_moo_buffer (view));
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}


static MooTextCursor
moo_text_view_get_text_cursor (G_GNUC_UNUSED MooTextView *view,
                               G_GNUC_UNUSED int          x,
                               G_GNUC_UNUSED int          y)
{
    return MOO_TEXT_CURSOR_TEXT;
}


void
moo_text_view_select_all (MooTextView *view)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_signal_emit_by_name (view, "select-all", TRUE);
}


static void
moo_text_view_set_property (GObject        *object,
                            guint           prop_id,
                            const GValue   *value,
                            GParamSpec     *pspec)
{
    MooTextView *view = MOO_TEXT_VIEW (object);
    GtkTextBuffer *buffer;

    switch (prop_id)
    {
        case PROP_BUFFER:
            buffer = g_value_get_object (value);

            if (!buffer)
            {
                buffer = GTK_TEXT_BUFFER (g_object_new (view->priv->buffer_type, (const char*) NULL));
            }
            else if (!g_type_is_a (G_OBJECT_TYPE (buffer), view->priv->buffer_type))
            {
                g_critical ("buffer '%s' is not of type '%s', ignoring it",
                            g_type_name (G_OBJECT_TYPE (buffer)),
                            g_type_name (view->priv->buffer_type));
                buffer = GTK_TEXT_BUFFER (g_object_new (view->priv->buffer_type, (const char*) NULL));
            }
            else
            {
                g_object_ref (buffer);
            }

            if (view->priv->buffer)
                g_critical ("%s: buffer already set", G_STRFUNC);

            gtk_text_view_set_buffer (GTK_TEXT_VIEW (view), buffer);
            view->priv->buffer = buffer;

/* Connect fold-toggled for visual features */
if (MOO_IS_TEXT_BUFFER (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view))))
{
    _moo_text_view_ensure_fold_header_tag (view);
    g_signal_connect (gtk_text_view_get_buffer (GTK_TEXT_VIEW (view)),
                     "fold-toggled",
                     G_CALLBACK (_moo_text_view_on_fold_toggled),
                     view);
}
            break;

        case PROP_INDENTER:
            moo_text_view_set_indenter (view, g_value_get_object (value));
            break;

        case PROP_TAB_WIDTH:
            moo_text_view_set_tab_width (view, g_value_get_uint (value));
            break;

        case PROP_HIGHLIGHT_MATCHING_BRACKETS:
            view->priv->highlight_matching_brackets = g_value_get_boolean (value);
            if (view->priv->constructed)
                g_object_set (get_buffer (view), "highlight-matching-brackets",
                              view->priv->highlight_matching_brackets, NULL);
            break;

        case PROP_HIGHLIGHT_MISMATCHING_BRACKETS:
            view->priv->highlight_mismatching_brackets = g_value_get_boolean (value);
            if (view->priv->constructed)
                g_object_set (get_buffer (view), "highlight-mismatching-brackets",
                              view->priv->highlight_mismatching_brackets, NULL);
            break;

        case PROP_HIGHLIGHT_CURRENT_LINE:
            moo_text_view_set_highlight_current_line (view, g_value_get_boolean (value));
            break;
        case PROP_HIGHLIGHT_CURRENT_LINE_UNFOCUSED:
            moo_text_view_set_highlight_current_line_unfocused (view, g_value_get_boolean (value));
            break;
        case PROP_DRAW_RIGHT_MARGIN:
            moo_text_view_set_draw_right_margin (view, g_value_get_boolean (value));
            break;
        case PROP_RIGHT_MARGIN_OFFSET:
            moo_text_view_set_right_margin_offset (view, g_value_get_uint (value));
            break;

        case PROP_CURRENT_LINE_COLOR:
            moo_text_view_set_current_line_color (view, g_value_get_string (value));
            break;
        case PROP_RIGHT_MARGIN_COLOR:
            moo_text_view_set_right_margin_color (view, g_value_get_string (value));
            break;

        case PROP_DRAW_WHITESPACE:
            set_draw_whitespace (view, g_value_get_flags (value));
            break;

        case PROP_MANAGE_CLIPBOARD:
            set_manage_clipboard (view, g_value_get_boolean (value));
            break;

        case PROP_SMART_HOME_END:
            view->priv->smart_home_end = g_value_get_boolean (value) != 0;
            g_object_notify (object, "smart-home-end");
            break;

        case PROP_ENABLE_HIGHLIGHT:
            moo_text_buffer_set_highlight (get_moo_buffer (view),
                                           g_value_get_boolean (value));
            g_object_notify (object, "enable-highlight");
            break;

        case PROP_SHOW_LINE_NUMBERS:
            moo_text_view_set_show_line_numbers (view, g_value_get_boolean (value));
            break;

        case PROP_SHOW_LINE_MARKS:
            set_show_line_marks (view, g_value_get_boolean (value));
            break;

        case PROP_ENABLE_FOLDING:
            set_enable_folding (view, g_value_get_boolean (value));
            break;

        case PROP_ENABLE_QUICK_SEARCH:
            view->priv->qs.enable = g_value_get_boolean (value) != 0;
            g_object_notify (object, "enable-quick-search");
            break;

        case PROP_QUICK_SEARCH_FLAGS:
            view->priv->qs.flags = g_value_get_flags (value);
            g_object_notify (object, "quick-search-flags");
            break;

        case PROP_AUTO_INDENT:
            view->priv->enter_indents = g_value_get_boolean (value);
            g_object_notify (object, "auto-indent");
            break;

        case PROP_TAB_INDENTS:
            view->priv->tab_indents = g_value_get_boolean (value);
            g_object_notify (object, "tab-indents");
            break;

        case PROP_BACKSPACE_INDENTS:
            view->priv->backspace_indents = g_value_get_boolean (value);
            g_object_notify (object, "backspace-indents");
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
moo_text_view_get_property (GObject        *object,
                            guint           prop_id,
                            GValue         *value,
                            GParamSpec     *pspec)
{
    MooTextView *view = MOO_TEXT_VIEW (object);
    gboolean val;

    switch (prop_id)
    {
        case PROP_BUFFER:
            g_value_set_object (value, get_buffer (view));
            break;
        case PROP_INDENTER:
            g_value_set_object (value, view->priv->indenter);
            break;
        case PROP_TAB_WIDTH:
            g_value_set_uint (value, view->priv->tab_width);
            break;
        case PROP_HIGHLIGHT_CURRENT_LINE_UNFOCUSED:
            g_value_set_boolean (value, view->priv->highlight_current_line_unfocused);
            break;
        case PROP_HIGHLIGHT_CURRENT_LINE:
            g_value_set_boolean (value, view->priv->color_settings[MOO_TEXT_VIEW_COLOR_CURRENT_LINE]);
            break;
        case PROP_DRAW_RIGHT_MARGIN:
            g_value_set_boolean (value, view->priv->color_settings[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN]);
            break;
        case PROP_RIGHT_MARGIN_OFFSET:
            g_value_set_uint (value, view->priv->right_margin_offset);
            break;
        case PROP_HIGHLIGHT_MATCHING_BRACKETS:
            g_object_get (get_buffer (view), "highlight-matching-brackets", &val, NULL);
            g_value_set_boolean (value, val);
            break;
        case PROP_HIGHLIGHT_MISMATCHING_BRACKETS:
            g_object_get (get_buffer (view), "highlight-mismatching-brackets", &val, NULL);
            g_value_set_boolean (value, val);
            break;
        case PROP_CURRENT_LINE_COLOR:
            g_value_set_string (value, view->priv->colors[MOO_TEXT_VIEW_COLOR_CURRENT_LINE]);
            break;
        case PROP_RIGHT_MARGIN_COLOR:
            g_value_set_string (value, view->priv->colors[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN]);
            break;
        case PROP_DRAW_WHITESPACE:
            g_value_set_flags (value, view->priv->draw_whitespace);
            break;
        case PROP_HAS_TEXT:
            g_value_set_boolean (value, moo_text_view_has_text (view));
            break;
        case PROP_HAS_SELECTION:
            g_value_set_boolean (value, moo_text_view_has_selection (view));
            break;
        case PROP_CAN_UNDO:
            g_value_set_boolean (value, moo_text_view_can_undo (view));
            break;
        case PROP_CAN_REDO:
            g_value_set_boolean (value, moo_text_view_can_redo (view));
            break;
        case PROP_MANAGE_CLIPBOARD:
            g_value_set_boolean (value, view->priv->manage_clipboard != 0);
            break;
        case PROP_SMART_HOME_END:
            g_value_set_boolean (value, view->priv->smart_home_end != 0);
            break;
        case PROP_ENABLE_HIGHLIGHT:
            g_value_set_boolean (value, moo_text_buffer_get_highlight (get_moo_buffer (view)));
            break;
        case PROP_SHOW_LINE_NUMBERS:
            g_value_set_boolean (value, view->priv->lm.show_numbers);
            break;
        case PROP_SHOW_LINE_MARKS:
            g_value_set_boolean (value, view->priv->lm.show_icons);
            break;
        case PROP_ENABLE_FOLDING:
            g_value_set_boolean (value, view->priv->enable_folding);
            break;
        case PROP_ENABLE_QUICK_SEARCH:
            g_value_set_boolean (value, view->priv->qs.enable);
            break;
        case PROP_QUICK_SEARCH_FLAGS:
            g_value_set_flags (value, view->priv->qs.flags);
            break;
        case PROP_AUTO_INDENT:
            g_value_set_boolean (value, view->priv->enter_indents != 0);
            break;
        case PROP_TAB_INDENTS:
            g_value_set_boolean (value, view->priv->tab_indents);
            break;
        case PROP_BACKSPACE_INDENTS:
            g_value_set_boolean (value, view->priv->backspace_indents != 0);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}


char *
moo_text_view_get_selection (GtkTextView *view)
{
    GtkTextBuffer *buf;
    GtkTextIter start, end;

    g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), NULL);

    buf = gtk_text_view_get_buffer (view);

    if (gtk_text_buffer_get_selection_bounds (buf, &start, &end))
        return gtk_text_buffer_get_slice (buf, &start, &end, TRUE);
    else
        return NULL;
}


gboolean
moo_text_view_has_selection (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), FALSE);
    return moo_text_buffer_has_selection (get_moo_buffer (view));
}


gboolean
moo_text_view_has_text (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), FALSE);
    return moo_text_buffer_has_text (get_moo_buffer (view));
}


char *
moo_text_view_get_text (GtkTextView *view)
{
    GtkTextBuffer *buf;
    GtkTextIter start, end;
    char *text;

    g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), NULL);

    buf = gtk_text_view_get_buffer (view);
    gtk_text_buffer_get_bounds (buf, &start, &end);
    text = gtk_text_buffer_get_slice (buf, &start, &end, TRUE);

    if (text && *text)
    {
        return text;
    }
    else
    {
        g_free (text);
        return NULL;
    }
}


static void
insert_text_cb (MooTextView        *view,
                GtkTextIter        *iter,
                gchar              *text,
                gint                len)
{
    if (view->priv->in_key_press && g_utf8_strlen (text, len) == 1)
    {
        view->priv->in_key_press = FALSE;
        view->priv->char_inserted = g_strdup (text);
        view->priv->char_inserted_offset = gtk_text_iter_get_offset (iter);
    }
}


void
_moo_text_view_check_char_inserted (MooTextView *view)
{
    if (view->priv->char_inserted)
    {
        gboolean result;
        GtkTextIter iter;

        gtk_text_buffer_get_iter_at_offset (get_buffer (view), &iter,
                                            view->priv->char_inserted_offset);

        g_signal_emit (view, signals[CHAR_INSERTED], 0,
                       &iter, view->priv->char_inserted,
                       &result);

        g_free (view->priv->char_inserted);
        view->priv->char_inserted = NULL;
    }
}


static gboolean
moo_text_view_char_inserted (MooTextView *view,
                             GtkTextIter *where,
                             const char  *character)
{
    if (view->priv->indenter)
    {
        GtkTextBuffer *buffer = get_buffer (view);
        gtk_text_buffer_begin_user_action (buffer);
        moo_indenter_character (view->priv->indenter,
                                character, where);
        gtk_text_buffer_end_user_action (buffer);
        return TRUE;
    }

    return FALSE;
}


void
moo_text_view_set_word_chars (MooTextView *view,
                              const char  *word_chars)
{
    guint i;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (!word_chars || g_utf8_validate (word_chars, -1, NULL));

    g_free (view->priv->word_chars);
    view->priv->word_chars = NULL;
    view->priv->n_word_chars = 0;

    if (!word_chars || !word_chars[0])
        return;

    view->priv->n_word_chars = g_utf8_strlen (word_chars, -1);
    view->priv->word_chars = g_new0 (gunichar, view->priv->n_word_chars);

    for (i = 0; *word_chars; i++, word_chars = g_utf8_next_char (word_chars))
        view->priv->word_chars[i] = g_utf8_get_char (word_chars);
}


static void
cursor_moved (MooTextView *view,
              GtkTextIter *where)
{
    g_signal_emit (view, signals[CURSOR_MOVED], 0, where);
}


static void
proxy_prop_notify (MooTextView *view,
                   GParamSpec  *pspec)
{
    g_object_notify (G_OBJECT (view), pspec->name);
}


static void
undo_ops_undo (MooUndoOps *obj)
{
    gboolean retval;
    g_signal_emit_by_name (obj, "undo", &retval);
}

static void
undo_ops_redo (MooUndoOps *obj)
{
    gboolean retval;
    g_signal_emit_by_name (obj, "redo", &retval);
}

static gboolean
undo_ops_can_undo (MooUndoOps *obj)
{
    return moo_text_view_can_undo (MOO_TEXT_VIEW (obj));
}

static gboolean
undo_ops_can_redo (MooUndoOps *obj)
{
    return moo_text_view_can_redo (MOO_TEXT_VIEW (obj));
}

static void
undo_ops_iface_init (MooUndoOpsIface *iface)
{
    iface->undo = undo_ops_undo;
    iface->redo = undo_ops_redo;
    iface->can_undo = undo_ops_can_undo;
    iface->can_redo = undo_ops_can_redo;
}

static void
proxy_notify_can_undo_redo (MooTextView *view,
                            GParamSpec  *pspec)
{
    if (strcmp (pspec->name, "can-undo") == 0)
        moo_undo_ops_can_undo_changed (G_OBJECT (view));
    else
        moo_undo_ops_can_redo_changed (G_OBJECT (view));

    g_object_notify (G_OBJECT (view), pspec->name);
}


MooIndenter*
moo_text_view_get_indenter (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), NULL);
    return view->priv->indenter;
}


void
moo_text_view_set_indenter (MooTextView *view,
                            MooIndenter *indenter)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (!indenter || MOO_IS_INDENTER (indenter));

    if (view->priv->indenter == indenter)
        return;

    if (view->priv->indenter)
        g_object_unref (view->priv->indenter);
    view->priv->indenter = indenter;
    if (view->priv->indenter)
        g_object_ref (view->priv->indenter);

    g_object_notify (G_OBJECT (view), "indenter");
}


typedef struct {
    MooTextView *view;
    int      line;
    int      character;
    gboolean visual;
} Scroll;


static int
iter_get_chars_in_line (const GtkTextIter *iter)
{
    GtkTextIter i = *iter;

    if (!gtk_text_iter_ends_line (&i))
        gtk_text_iter_forward_to_line_end (&i);

    return gtk_text_iter_get_line_offset (&i);
}

static gboolean
do_move_cursor (Scroll *scroll)
{
    GtkTextBuffer *buffer;
    GtkTextIter iter;

    g_return_val_if_fail (scroll != NULL, FALSE);
    g_return_val_if_fail (GTK_IS_TEXT_VIEW (scroll->view), FALSE);
    g_return_val_if_fail (scroll->line >= 0, FALSE);

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (scroll->view));

    if (scroll->line >= gtk_text_buffer_get_line_count (buffer))
        scroll->line = gtk_text_buffer_get_line_count (buffer) - 1;

    gtk_text_buffer_get_iter_at_line (buffer, &iter, scroll->line);

    if (scroll->character >= 0)
    {
        if (scroll->visual)
        {
            int line_len = moo_text_iter_get_visual_line_length (&iter, 8);

            if (scroll->character > line_len)
                scroll->character = line_len;
            else if (scroll->character < 0)
                scroll->character = 0;

            if (scroll->character == line_len)
            {
                if (!gtk_text_iter_ends_line (&iter))
                    gtk_text_iter_forward_to_line_end (&iter);
            }
            else
            {
                moo_text_iter_set_visual_line_offset (&iter, scroll->character, 8);
            }
        }
        else
        {
            int line_len = iter_get_chars_in_line (&iter);

            if (scroll->character > line_len)
                scroll->character = line_len;
            else if (scroll->character < 0)
                scroll->character = 0;

            if (scroll->character == line_len)
            {
                if (!gtk_text_iter_ends_line (&iter))
                    gtk_text_iter_forward_to_line_end (&iter);
            }
            else
            {
                gtk_text_iter_set_line_offset (&iter, scroll->character);
            }
        }
    }

    gtk_text_buffer_place_cursor (buffer, &iter);
    gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (scroll->view),
                                  gtk_text_buffer_get_insert (buffer),
                                  0.2, FALSE, 0, 0);

    if (MOO_IS_TEXT_VIEW (scroll->view))
    {
        MooTextView *view = MOO_TEXT_VIEW (scroll->view);

        if (view->priv->move_cursor_idle)
        {
            g_source_remove (view->priv->move_cursor_idle);
            view->priv->move_cursor_idle = 0;
        }
    }

    return FALSE;
}


void
moo_text_view_move_cursor (MooTextView *view,
                           int          line,
                           int          offset,
                           gboolean     offset_visual,
                           gboolean     in_idle)
{
    Scroll scroll;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    scroll.view = view;
    scroll.line = line;
    scroll.character = offset;
    scroll.visual = offset_visual;

    do_move_cursor (&scroll);

    if (in_idle)
    {
        MooTextView *mview;

        g_return_if_fail (MOO_IS_TEXT_VIEW (view));

        mview = MOO_TEXT_VIEW (view);
        mview->priv->move_cursor_idle =
            g_idle_add_full (G_PRIORITY_HIGH_IDLE + 9, /* between gtktextview's first validate priority and
                                                          * GTK_PRIORITY_RESIZE */
                                       (GSourceFunc) do_move_cursor,
                                       g_memdup (&scroll, sizeof scroll),
                                       g_free);
    }
}


void
moo_text_view_get_cursor (GtkTextView *view,
                          GtkTextIter *iter)
{
    GtkTextBuffer *buffer;

    g_return_if_fail (GTK_IS_TEXT_VIEW (view));
    g_return_if_fail (iter != NULL);

    buffer = gtk_text_view_get_buffer (view);
    gtk_text_buffer_get_iter_at_mark (buffer, iter,
                                      gtk_text_buffer_get_insert (buffer));
}


int
moo_text_view_get_cursor_line (GtkTextView *view)
{
    GtkTextIter iter;

    g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), -1);

    moo_text_view_get_cursor (view, &iter);
    return gtk_text_iter_get_line (&iter);
}


static GtkTextBuffer*
get_buffer (MooTextView *view)
{
    return gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
}


static MooTextBuffer*
get_moo_buffer (MooTextView *view)
{
    return MOO_TEXT_BUFFER (get_buffer (view));
}


static GtkTextMark*
get_insert (MooTextView    *view)
{
    return gtk_text_buffer_get_insert (get_buffer (view));
}


static void
moo_text_view_set_color_setting (MooTextView     *view,
                                 const char      *prop,
                                 gboolean         setting,
                                 MooTextViewColor color_num)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (view->priv->color_settings[color_num] != setting)
    {
        view->priv->color_settings[color_num] = setting;
        invalidate_gcs (view);
        gtk_widget_queue_draw (GTK_WIDGET (view));
        g_object_notify (G_OBJECT (view), prop);
    }
}

void
moo_text_view_set_highlight_current_line (MooTextView *view,
                                          gboolean     highlight)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    moo_text_view_set_color_setting (view, "highlight-current-line", highlight,
                                     MOO_TEXT_VIEW_COLOR_CURRENT_LINE);
}

void
moo_text_view_set_highlight_current_line_unfocused (MooTextView *view,
                                                    gboolean     highlight)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (view->priv->highlight_current_line_unfocused != highlight)
    {
        view->priv->highlight_current_line_unfocused = highlight;
        gtk_widget_queue_draw (GTK_WIDGET (view));
        g_object_notify (G_OBJECT (view), "highlight-current-line-unfocused");
    }
}

void
moo_text_view_set_draw_right_margin (MooTextView *view,
                                     gboolean     draw)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    moo_text_view_set_color_setting (view, "draw-right-margin", draw,
                                     MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN);
}


void
moo_text_view_set_right_margin_offset (MooTextView *view,
                                       guint        offset)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (offset > 0);

    if (view->priv->right_margin_offset != offset)
    {
        view->priv->right_margin_offset = offset;

        invalidate_right_margin (view);

        if (view->priv->color_settings[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN])
            gtk_widget_queue_draw (GTK_WIDGET (view));

        g_object_notify (G_OBJECT (view), "right-margin-offset");
    }
}


static void
moo_text_view_set_color (MooTextView      *view,
                         MooTextViewColor  color_num,
                         const char       *color,
                         const char       *propname)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (color_num < MOO_TEXT_VIEW_N_COLORS);

    MOO_ASSIGN_STRING (view->priv->colors[color_num], color);

    invalidate_gcs (view);

    if (gtk_widget_is_drawable (GTK_WIDGET (view)) && view->priv->color_settings[color_num])
        gtk_widget_queue_draw (GTK_WIDGET (view));

    g_object_notify (G_OBJECT (view), propname);
}

void
moo_text_view_set_current_line_color (MooTextView *view,
                                      const char  *color)
{
    moo_text_view_set_color (view, MOO_TEXT_VIEW_COLOR_CURRENT_LINE,
                             color, "current-line-color");
}

void
moo_text_view_set_right_margin_color (MooTextView *view,
                                      const char  *color)
{
    moo_text_view_set_color (view, MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN,
                             color, "right-margin-color");
}


/********************************************************************/
/* Clipboard
 */

enum {
    TARGET_TEXT,
};

static const GtkTargetEntry targets[] = {
    { (char*) "STRING", 0, TARGET_TEXT },
    { (char*) "TEXT",   0, TARGET_TEXT },
    { (char*) "COMPOUND_TEXT", 0, TARGET_TEXT },
    { (char*) "UTF8_STRING", 0, TARGET_TEXT }
};


static void
add_selection_clipboard (MooTextView *view)
{
    GtkClipboard *clipboard;
    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (view), GDK_SELECTION_PRIMARY);
    gtk_text_buffer_add_selection_clipboard (get_buffer (view), clipboard);
}

static void
remove_selection_clipboard (MooTextView *view)
{
    GtkClipboard *clipboard;
    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (view), GDK_SELECTION_PRIMARY);
    gtk_text_buffer_remove_selection_clipboard (get_buffer (view), clipboard);
}


static void
clipboard_get_selection (G_GNUC_UNUSED GtkClipboard *clipboard,
                         GtkSelectionData *selection_data,
                         G_GNUC_UNUSED guint info,
                         gpointer data)
{
    MooTextView *view = data;
    GtkTextIter start, end;

    if (gtk_text_buffer_get_selection_bounds (get_buffer (view), &start, &end))
    {
        char *text = gtk_text_iter_get_text (&start, &end);
        gtk_selection_data_set_text (selection_data, text, -1);
        g_free (text);
    }
}


static void
clear_primary (MooTextView *view)
{
    GtkClipboard *clipboard;

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (view),
                                          GDK_SELECTION_PRIMARY);

    if (gtk_clipboard_get_owner (clipboard) == G_OBJECT (view))
        gtk_clipboard_clear (clipboard);
}


static void
selection_changed (MooTextView   *view,
                   MooTextBuffer *buffer)
{
    GtkClipboard *clipboard;

    if (!gtk_widget_get_realized (GTK_WIDGET (view)) || !gtk_widget_has_focus (GTK_WIDGET (view)) || !view->priv->manage_clipboard)
        return;

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (view),
                                          GDK_SELECTION_PRIMARY);

    if (moo_text_buffer_has_selection (buffer))
        gtk_clipboard_set_with_owner (clipboard, targets,
                                      G_N_ELEMENTS (targets),
                                      clipboard_get_selection,
                                      NULL, G_OBJECT (view));
}


static void
set_manage_clipboard (MooTextView    *view,
                      gboolean        manage)
{
    if (BOOL_CMP (view->priv->manage_clipboard, manage))
        return;

    view->priv->manage_clipboard = manage;

    if (gtk_widget_get_realized (GTK_WIDGET (view)))
    {
        if (manage)
        {
            remove_selection_clipboard (view);
            selection_changed (view, get_moo_buffer (view));
        }
        else
        {
            clear_primary (view);
            add_selection_clipboard (view);
        }
    }

    g_object_notify (G_OBJECT (view), "manage-clipboard");
}


static void
get_clipboard (G_GNUC_UNUSED GtkClipboard *clipboard,
               GtkSelectionData *selection_data,
               G_GNUC_UNUSED guint info,
               gpointer data)
{
    MooTextViewClipboard *contents = data;
    gtk_selection_data_set_text (selection_data, contents->text, -1);
}


static void
clear_clipboard (G_GNUC_UNUSED GtkClipboard *clipboard,
                 gpointer data)
{
    MooTextViewClipboard *contents = data;

    if (contents->view && contents->view->priv->clipboard == contents)
        contents->view->priv->clipboard = NULL;

    g_free (contents->text);
    g_slice_free (MooTextViewClipboard, contents);
}


static void
moo_text_view_cut_or_copy (GtkTextView *text_view,
                           gboolean     delete,
                           GdkAtom      clipboard_type)
{
    GtkTextBuffer *buffer;
    GtkTextIter start, end;
    GtkClipboard *clipboard;
    MooTextViewClipboard *contents;
    MooTextView *view = MOO_TEXT_VIEW (text_view);

    buffer = gtk_text_view_get_buffer (text_view);

    if (!gtk_text_buffer_get_selection_bounds (buffer, &start, &end))
        return;

    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (text_view),
                                          clipboard_type);

    contents = g_slice_new0 (MooTextViewClipboard);
    contents->text = gtk_text_buffer_get_slice (buffer, &start, &end, TRUE);
    contents->view = view;

    if (view->priv->clipboard)
        view->priv->clipboard->view = NULL;
    view->priv->clipboard = contents;

    if (!gtk_clipboard_set_with_data (clipboard, targets, G_N_ELEMENTS (targets),
                                      get_clipboard, clear_clipboard,
                                      contents))
        return;

    gtk_clipboard_set_can_store (clipboard, targets + 1,
                                 G_N_ELEMENTS (targets) - 1);

    if (delete)
    {
        gtk_text_buffer_begin_user_action (buffer);
        gtk_text_buffer_delete (buffer, &start, &end);
        gtk_text_buffer_end_user_action (buffer);
        gtk_text_view_scroll_mark_onscreen (text_view,
                                            gtk_text_buffer_get_insert (buffer));
    }
}

void
_moo_text_view_ensure_primary (GtkTextView *text_view)
{
    moo_text_view_cut_or_copy (text_view, FALSE, GDK_SELECTION_PRIMARY);
}

static void
moo_text_view_copy_clipboard (GtkTextView *text_view)
{
    /* If box selection is active, copy box text instead */
    if (MOO_TEXT_VIEW (text_view)->priv->box_sel.active)
    {
        moo_text_view_box_copy (MOO_TEXT_VIEW (text_view));
        return;
    }

    moo_text_view_cut_or_copy (text_view, FALSE, GDK_SELECTION_CLIPBOARD);
    MOO_TEXT_VIEW (text_view)->priv->box_sel.box_copied = FALSE;
}


static void
moo_text_view_cut_clipboard (GtkTextView *text_view)
{
    MooTextView *view = MOO_TEXT_VIEW (text_view);

    /* If box selection is active, copy then delete the selected rectangle */
    if (view->priv->box_sel.active)
    {
        moo_text_view_box_copy (view);
        box_sel_delete (view);
        box_sel_clear (view);
        return;
    }

    view->priv->box_sel.box_copied = FALSE;
    moo_text_view_cut_or_copy (text_view, TRUE, GDK_SELECTION_CLIPBOARD);
}


static void
paste_text (GtkTextView *text_view,
            const char  *text)
{
    GtkTextBuffer *buffer;
    GtkTextIter start, end;

    g_return_if_fail (text != NULL);

    buffer = gtk_text_view_get_buffer (text_view);

    if (gtk_text_buffer_get_selection_bounds (buffer, &start, &end))
        gtk_text_buffer_delete (buffer, &start, &end);

    gtk_text_buffer_insert (buffer, &end, text, -1);
}


static gboolean
paste_refresh_idle (gpointer data)
{
    GtkTextView *text_view = GTK_TEXT_VIEW (data);

    if (GTK_IS_TEXT_VIEW (text_view) && gtk_widget_get_realized (GTK_WIDGET (text_view)))
    {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer (text_view);
        GtkTextMark *insert = gtk_text_buffer_get_insert (buffer);
        GdkRectangle visible, cursor_loc;
        GtkTextIter iter;

        gtk_text_view_get_visible_rect (text_view, &visible);
        gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
        gtk_text_view_get_iter_location (text_view, &iter, &cursor_loc);

        /* Only scroll if the cursor is outside the visible area */
        if (cursor_loc.y < visible.y ||
            cursor_loc.y + cursor_loc.height > visible.y + visible.height)
        {
            gtk_text_view_scroll_to_mark (text_view, insert,
                                          0.05, FALSE, 0.0, 0.0);
        }

        gtk_widget_queue_draw (GTK_WIDGET (text_view));
    }

    g_object_unref (text_view);
    return G_SOURCE_REMOVE;
}

/* Copy box selection to clipboard */
static void
moo_text_view_box_copy (MooTextView *view)
{
    GtkTextView *tv = GTK_TEXT_VIEW (view);
    GtkClipboard *clipboard;
    char *text;

    if (!view->priv->box_sel.active)
        return;

    text = box_sel_get_text (tv,
        view->priv->box_sel.anchor_x, view->priv->box_sel.anchor_y,
        view->priv->box_sel.current_x, view->priv->box_sel.current_y);

    if (text && text[0])
    {
        clipboard = gtk_widget_get_clipboard (GTK_WIDGET (view),
                                              GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text (clipboard, text, -1);
        view->priv->box_sel.box_copied = TRUE;
    }

    g_free (view->priv->box_sel.copied_text);
    view->priv->box_sel.copied_text = text;
}


/* ------------------------------------------------------------------ */
/* Box (column) paste                                                  */
/* ------------------------------------------------------------------ */

static void
moo_text_view_box_paste_at_cursor (GtkTextView *text_view,
                                   const char  *box_text)
{
    GtkTextBuffer *buffer;
    GtkTextIter cursor;
    int cursor_line, cursor_col;
    char **lines;
    int n_lines, i;

    if (!box_text || !box_text[0])
        return;

    buffer = gtk_text_view_get_buffer (text_view);
    gtk_text_buffer_get_iter_at_mark (buffer, &cursor,
        gtk_text_buffer_get_insert (buffer));
    cursor_line = gtk_text_iter_get_line (&cursor);
    cursor_col = gtk_text_iter_get_line_offset (&cursor);

    lines = g_strsplit (box_text, "\n", -1);
    n_lines = (int) g_strv_length (lines);

    gtk_text_buffer_begin_user_action (buffer);

    for (i = 0; i < n_lines; i++)
    {
        int target_line = cursor_line + i;
        GtkTextIter ls, insert_pos;
        int current_line_len;
        const char *paste_line = lines[i];

        /* Append new lines if needed */
        if (target_line >= gtk_text_buffer_get_line_count (buffer))
        {
            GtkTextIter end_iter;
            gtk_text_buffer_get_end_iter (buffer, &end_iter);
            gtk_text_buffer_insert (buffer, &end_iter, "\n", -1);
        }

        gtk_text_buffer_get_iter_at_line (buffer, &ls, target_line);
        insert_pos = ls;
        if (!gtk_text_iter_ends_line (&insert_pos))
            gtk_text_iter_forward_to_line_end (&insert_pos);
        current_line_len = gtk_text_iter_get_line_offset (&insert_pos);

        /* Pad with spaces if line is shorter than cursor column */
        if (current_line_len < cursor_col)
        {
            int pad = cursor_col - current_line_len;
            char *spaces = g_strnfill (pad, ' ');
            gtk_text_buffer_insert (buffer, &insert_pos, spaces, -1);
            g_free (spaces);
        }

        /* Position at cursor column for insertion */
        gtk_text_buffer_get_iter_at_line (buffer, &ls, target_line);
        insert_pos = ls;
        gtk_text_iter_set_line_offset (&insert_pos, cursor_col);

        /* Insert the paste line text */
        gtk_text_buffer_insert (buffer, &insert_pos, paste_line, -1);
    }

    gtk_text_buffer_end_user_action (buffer);

    /* Place cursor after the pasted block */
    {
        GtkTextIter new_cursor;
        int last_paste_line = cursor_line + n_lines - 1;
        if (last_paste_line >= gtk_text_buffer_get_line_count (buffer))
            last_paste_line = gtk_text_buffer_get_line_count (buffer) - 1;
        gtk_text_buffer_get_iter_at_line (buffer, &new_cursor, last_paste_line);
        if (n_lines > 0)
        {
            int target_col = cursor_col +
                (int) g_utf8_strlen (lines[n_lines - 1], -1);
            GtkTextIter le = new_cursor;
            if (!gtk_text_iter_ends_line (&le))
                gtk_text_iter_forward_to_line_end (&le);
            if (target_col <= gtk_text_iter_get_line_offset (&le))
                gtk_text_iter_set_line_offset (&new_cursor, target_col);
            else
                new_cursor = le;
        }
        gtk_text_buffer_place_cursor (buffer, &new_cursor);
    }

    g_strfreev (lines);
}

static void
moo_text_view_box_paste_into_selection (MooTextView *view,
                                        const char  *box_text)
{
    GtkTextView *text_view = GTK_TEXT_VIEW (view);
    GtkTextBuffer *buffer;
    int ax, ay, bx, by;
    int first_line, last_line, sel_lines;
    int left_bx, right_bx;
    int *col_lefts, *col_rights;
    char **lines;
    int n_lines, i;

    if (!box_text || !box_text[0])
        return;

    ax = view->priv->box_sel.anchor_x;
    ay = view->priv->box_sel.anchor_y;
    bx = view->priv->box_sel.current_x;
    by = view->priv->box_sel.current_y;

    buffer = gtk_text_view_get_buffer (text_view);

    {
        GtkTextIter ia, ib;
        gtk_text_view_get_iter_at_location (text_view, &ia, ax, ay);
        gtk_text_view_get_iter_at_location (text_view, &ib, bx, by);
        first_line = gtk_text_iter_get_line (&ia);
        last_line = gtk_text_iter_get_line (&ib);
        if (first_line > last_line)
        {
            int t = first_line; first_line = last_line; last_line = t;
        }
    }

    left_bx = (ax < bx) ? ax : bx;
    right_bx = (ax > bx) ? ax : bx;
    sel_lines = last_line - first_line + 1;

    lines = g_strsplit (box_text, "\n", -1);
    n_lines = (int) g_strv_length (lines);

    /* Pre-compute column offsets for all selected lines BEFORE any
     * buffer modifications, since pixel->column mapping changes
     * after inserts/deletes. */
    col_lefts = g_new0 (int, sel_lines);
    col_rights = g_new0 (int, sel_lines);
    for (i = 0; i < sel_lines; i++)
    {
        col_lefts[i]  = box_sel_visual_col_at_x (text_view, first_line + i, left_bx,  FALSE);
        col_rights[i] = box_sel_visual_col_at_x (text_view, first_line + i, right_bx, TRUE);
    }

    gtk_text_buffer_begin_user_action (buffer);

    /* Process line by line from BOTTOM to TOP to avoid line number shifts.
     * But we need to handle three cases:
     *   - Lines within both source and target range: delete target, insert source
     *   - Extra source lines (n_lines > sel_lines): insert new lines
     *   - Extra target lines (sel_lines > n_lines): just delete target columns
     *
     * Process in two passes:
     *   Pass 1: Handle existing target lines (bottom to top)
     *   Pass 2: Insert extra source lines (if any) below the target range
     */

    /* Pass 1: Process existing target lines (bottom to top) */
    for (i = sel_lines - 1; i >= 0; i--)
    {
        int target_line = first_line + i;
        int cl = col_lefts[i];
        int cr = col_rights[i];
        GtkTextIter ls, le, del_start, del_end;
        int line_len;

        gtk_text_buffer_get_iter_at_line (buffer, &ls, target_line);
        le = ls;
        if (!gtk_text_iter_ends_line (&le))
            gtk_text_iter_forward_to_line_end (&le);
        line_len = gtk_text_iter_get_line_offset (&le);

        /* Delete the selected column range on this line */
        if (cl < line_len)
        {
            del_start = ls;
            gtk_text_iter_set_line_offset (&del_start, cl);
            if (cr <= line_len)
            {
                del_end = ls;
                gtk_text_iter_set_line_offset (&del_end, cr);
            }
            else
            {
                del_end = le;
            }
            if (gtk_text_iter_compare (&del_start, &del_end) < 0)
                gtk_text_buffer_delete (buffer, &del_start, &del_end);
        }

        /* Insert source line text (if this line has a corresponding source line) */
        if (i < n_lines)
        {
            const char *paste_line = lines[i];

            /* Refresh iterators after delete */
            gtk_text_buffer_get_iter_at_line (buffer, &ls, target_line);
            le = ls;
            if (!gtk_text_iter_ends_line (&le))
                gtk_text_iter_forward_to_line_end (&le);
            line_len = gtk_text_iter_get_line_offset (&le);

            /* Pad with spaces if line is too short to reach col_left */
            if (line_len < cl)
            {
                int pad = cl - line_len;
                char *spaces = g_strnfill (pad, ' ');
                gtk_text_buffer_insert (buffer, &le, spaces, -1);
                g_free (spaces);
                /* Refresh */
                gtk_text_buffer_get_iter_at_line (buffer, &ls, target_line);
                le = ls;
                if (!gtk_text_iter_ends_line (&le))
                    gtk_text_iter_forward_to_line_end (&le);
            }

            /* Insert at col_left */
            {
                GtkTextIter ins = ls;
                int cur_len;
                le = ls;
                if (!gtk_text_iter_ends_line (&le))
                    gtk_text_iter_forward_to_line_end (&le);
                cur_len = gtk_text_iter_get_line_offset (&le);

                if (cl <= cur_len)
                    gtk_text_iter_set_line_offset (&ins, cl);
                else
                    ins = le;

                gtk_text_buffer_insert (buffer, &ins, paste_line, -1);
            }
        }
        /* else: target line beyond source lines — columns already deleted, done */
    }

    /* Pass 2: Insert extra source lines if source has more lines than target */
    if (n_lines > sel_lines)
    {
        /* Insert after the last target line */
        int insert_after = last_line;
        int cl_first = col_lefts[0];  /* Use first line's column offset */

        for (i = sel_lines; i < n_lines; i++)
        {
            GtkTextIter insert_line_iter;
            int ins_line = insert_after + 1 + (i - sel_lines);
            const char *paste_line = lines[i];
            char *padded;

            /* Insert a new line at the end of insert_after + offset */
            if (ins_line >= gtk_text_buffer_get_line_count (buffer))
            {
                GtkTextIter end_iter;
                gtk_text_buffer_get_end_iter (buffer, &end_iter);
                gtk_text_buffer_insert (buffer, &end_iter, "\n", -1);
            }
            else
            {
                /* Insert a blank line before ins_line */
                gtk_text_buffer_get_iter_at_line (buffer, &insert_line_iter, ins_line);
                gtk_text_buffer_insert (buffer, &insert_line_iter, "\n", -1);
            }

            /* Now fill the new line: pad to col_left, then paste text */
            gtk_text_buffer_get_iter_at_line (buffer, &insert_line_iter, ins_line);
            if (cl_first > 0)
            {
                char *spaces = g_strnfill (cl_first, ' ');
                padded = g_strconcat (spaces, paste_line, NULL);
                g_free (spaces);
            }
            else
            {
                padded = g_strdup (paste_line);
            }

            gtk_text_buffer_insert (buffer, &insert_line_iter, padded, -1);
            g_free (padded);
        }
    }

    gtk_text_buffer_end_user_action (buffer);

    /* Clear box selection */
    box_sel_clear (view);

    g_free (col_lefts);
    g_free (col_rights);
    g_strfreev (lines);
}


static void
moo_text_view_paste_clipboard (GtkTextView *text_view)
{
    /* Check if the last copy was a box selection — do box paste */
    {
        MooTextView *mview = MOO_TEXT_VIEW (text_view);
        if (mview->priv->box_sel.box_copied && mview->priv->box_sel.copied_text)
        {
            if (mview->priv->box_sel.active)
                moo_text_view_box_paste_into_selection (mview,
                    mview->priv->box_sel.copied_text);
            else
                moo_text_view_box_paste_at_cursor (text_view,
                    mview->priv->box_sel.copied_text);
            gtk_text_view_scroll_mark_onscreen (text_view,
                gtk_text_buffer_get_insert (
                    gtk_text_view_get_buffer (text_view)));
            return;
        }
    }

    char *text;
    GtkTextBuffer *buffer;
    GtkClipboard *clipboard;

    buffer = gtk_text_view_get_buffer (text_view);
    clipboard = gtk_widget_get_clipboard (GTK_WIDGET (text_view), GDK_SELECTION_CLIPBOARD);

    /* Record the cursor line before the paste so we can limit the LL
     * rescan to only the inserted lines rather than the whole buffer. */
    int paste_first_line = 0;
    {
        GtkTextIter pre_iter;
        gtk_text_buffer_get_iter_at_mark (buffer, &pre_iter,
                                          gtk_text_buffer_get_insert (buffer));
        paste_first_line = gtk_text_iter_get_line (&pre_iter);
    }

    gtk_text_buffer_begin_user_action (buffer);

    if ((text = gtk_clipboard_wait_for_text (clipboard)))
    {
        paste_text (text_view, text);
        g_free (text);
    }

    gtk_text_buffer_end_user_action (buffer);

    /* Apply long-line truncation only to the pasted region.
     * The old code used moo_ll_apply_range(buffer, 0, _n-1) which rescanned
     * the ENTIRE buffer on every paste — O(n) for a 25 k-line file. */
    {
        extern void moo_ll_apply_range (GtkTextBuffer *buf, int first, int last);
        /* Ensure nowrap flag is set for new documents opened via paste */
        if (!GPOINTER_TO_INT (g_object_get_data (G_OBJECT (buffer), "moo-nowrap-mode")))
        {
            GtkWrapMode wm = gtk_text_view_get_wrap_mode (text_view);
            if (wm == GTK_WRAP_NONE)
                g_object_set_data (G_OBJECT (buffer), "moo-nowrap-mode",
                                   GINT_TO_POINTER (1));
        }
        if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (buffer), "moo-nowrap-mode")))
        {
            GtkTextIter post_iter;
            int paste_last_line;
            gtk_text_buffer_get_iter_at_mark (buffer, &post_iter,
                                              gtk_text_buffer_get_insert (buffer));
            paste_last_line = gtk_text_iter_get_line (&post_iter);
            moo_ll_apply_range (buffer, paste_first_line, paste_last_line);
        }
    }


    /* Cursor is now at the end of the inserted text.
     * Only scroll if it's not already visible. */
    {
        GdkRectangle visible, cursor_loc;
        GtkTextIter iter;
        GtkTextMark *insert = gtk_text_buffer_get_insert (buffer);

        gtk_text_view_get_visible_rect (text_view, &visible);
        gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
        gtk_text_view_get_iter_location (text_view, &iter, &cursor_loc);

        if (cursor_loc.y < visible.y ||
            cursor_loc.y + cursor_loc.height > visible.y + visible.height)
        {
            gtk_text_view_scroll_mark_onscreen (text_view, insert);
        }
    }

    /* Deferred redraw: GTK may not have finished layout yet after a
     * large paste.  The idle callback also re-checks visibility and
     * scrolls if needed, then forces a repaint. */
    g_object_ref (text_view);
    g_idle_add (paste_refresh_idle, text_view);
}


/********************************************************************/
/* Drawing and stuff
 */

/* XXX: workaround for http://bugzilla.gnome.org/show_bug.cgi?id=336796 */
static void
lower_border_window (GtkTextView   *view,
                     MooTextViewPos pos)
{
    GdkWindow *window;

    g_return_if_fail (pos < 4);

    window = gtk_text_view_get_window (view, window_types[pos]);

    if (window)
        window = gdk_window_get_parent (window);

    if (window)
        gdk_window_lower (window);
}


/* ------------------------------------------------------------------ */
/* Ctrl+Scroll font zoom (per-view, temporary)                        */
/* ------------------------------------------------------------------ */

#define MOO_FONT_ZOOM_MIN 4
#define MOO_FONT_ZOOM_MAX 72
#define MOO_FONT_ZOOM_STEP 1

static void
moo_text_view_change_font_size (MooTextView *view, int delta)
{
    PangoContext *ctx;
    const PangoFontDescription *current;
    PangoFontDescription *fd;
    int size;

    ctx = gtk_widget_get_pango_context (GTK_WIDGET (view));
    if (!ctx) return;

    current = pango_context_get_font_description (ctx);
    if (!current) return;

    fd = pango_font_description_copy (current);
    size = pango_font_description_get_size (fd);

    /* Handle both scaled (size in points * PANGO_SCALE) and absolute sizes */
    if (pango_font_description_get_size_is_absolute (fd))
    {
        size += delta * PANGO_SCALE;
        if (size < MOO_FONT_ZOOM_MIN * PANGO_SCALE)
            size = MOO_FONT_ZOOM_MIN * PANGO_SCALE;
        if (size > MOO_FONT_ZOOM_MAX * PANGO_SCALE)
            size = MOO_FONT_ZOOM_MAX * PANGO_SCALE;
        pango_font_description_set_absolute_size (fd, size);
    }
    else
    {
        size += delta * PANGO_SCALE;
        if (size < MOO_FONT_ZOOM_MIN * PANGO_SCALE)
            size = MOO_FONT_ZOOM_MIN * PANGO_SCALE;
        if (size > MOO_FONT_ZOOM_MAX * PANGO_SCALE)
            size = MOO_FONT_ZOOM_MAX * PANGO_SCALE;
        pango_font_description_set_size (fd, size);
    }

    gtk_widget_override_font (GTK_WIDGET (view), fd);
    pango_font_description_free (fd);

    /* Update tab width after font change */
    update_tab_width (view);
    update_left_margin (view);
}

void
moo_text_view_reset_font_size (MooTextView *view)
{
    /* Remove the font override — reverts to the style/prefs font */
    gtk_widget_override_font (GTK_WIDGET (view), NULL);
    update_tab_width (view);
    update_left_margin (view);
}

static gboolean
moo_text_view_scroll_event (GtkWidget      *widget,
                            GdkEventScroll *event)
{
    MooTextView *view = MOO_TEXT_VIEW (widget);

    if (event->state & GDK_CONTROL_MASK)
    {
        switch (event->direction)
        {
            case GDK_SCROLL_UP:
                moo_text_view_change_font_size (view, MOO_FONT_ZOOM_STEP);
                return TRUE;
            case GDK_SCROLL_DOWN:
                moo_text_view_change_font_size (view, -MOO_FONT_ZOOM_STEP);
                return TRUE;
            case GDK_SCROLL_SMOOTH:
            {
                /* Handle smooth scrolling (trackpad) */
                if (event->delta_y < 0)
                    moo_text_view_change_font_size (view, MOO_FONT_ZOOM_STEP);
                else if (event->delta_y > 0)
                    moo_text_view_change_font_size (view, -MOO_FONT_ZOOM_STEP);
                return TRUE;
            }
            default:
                break;
        }
    }

    /* Chain up for normal scrolling */
    return GTK_WIDGET_CLASS (moo_text_view_parent_class)->scroll_event
           ? GTK_WIDGET_CLASS (moo_text_view_parent_class)->scroll_event (widget, event)
           : FALSE;
}


static void
moo_text_view_realize (GtkWidget *widget)
{
    MooTextView *view = MOO_TEXT_VIEW (widget);
    guint i;

    GTK_WIDGET_CLASS(moo_text_view_parent_class)->realize (widget);

    update_tab_width (view);
    update_left_margin (view);

    if (view->priv->manage_clipboard)
    {
        remove_selection_clipboard (view);
        selection_changed (view, get_moo_buffer (view));
    }

    /* workaround for http://bugzilla.gnome.org/show_bug.cgi?id=336796 */
    for (i = 0; i < 4; ++i)
    {
        if (view->priv->children[i] && gtk_widget_get_visible (GTK_WIDGET (view->priv->children[i])))
            lower_border_window (GTK_TEXT_VIEW (view), i);
    }

    _moo_text_view_update_text_cursor (view, -1, -1);
}


static void
moo_text_view_unrealize (GtkWidget *widget)
{
    MooTextView *view = MOO_TEXT_VIEW (widget);

    g_slist_foreach (view->priv->line_marks, (GFunc)(void(*)(void)) _moo_line_mark_unrealize, NULL);
    g_object_set_data (G_OBJECT (widget), "moo-line-mark-icons", NULL);
    g_object_set_data (G_OBJECT (widget), "moo-line-mark-colors", NULL);

    if (view->priv->update_idle)
        g_source_remove (view->priv->update_idle);
    view->priv->update_idle = 0;

    if (view->priv->update_n_lines_idle)
        g_source_remove (view->priv->update_n_lines_idle);
    view->priv->update_n_lines_idle = 0;

    g_free (view->priv->update_rectangle);
    view->priv->update_rectangle = NULL;

    invalidate_gcs (view);

    if (view->priv->manage_clipboard)
    {
        clear_primary (view);
        add_selection_clipboard (view);
    }

    if (view->priv->dnd.scroll_timeout)
    {
        g_source_remove (view->priv->dnd.scroll_timeout);
        view->priv->dnd.scroll_timeout = 0;
    }

    GTK_WIDGET_CLASS(moo_text_view_parent_class)->unrealize (widget);
}


static void
invalidate_gcs (MooTextView *view)
{
    int i;

    for (i = 0; i < MOO_TEXT_VIEW_N_COLORS; ++i)
    {
        if (view->priv->gcs[i])
            gdk_rgba_free(view->priv->gcs[i]);
        view->priv->gcs[i] = NULL;
    }
}

static void
update_gc (MooTextView     *view,
           MooTextViewColor color_num)
{
    GdkRGBA color;
    GtkWidget *widget = GTK_WIDGET (view);
    GdkWindow *window;

    if (!gtk_widget_get_realized (widget))
        return;

    g_return_if_fail (color_num < MOO_TEXT_VIEW_N_COLORS);

    if (!view->priv->color_settings[color_num])
    {
        if (view->priv->gcs[color_num])
        {
            gdk_rgba_free(view->priv->gcs[color_num]);
            view->priv->gcs[color_num] = NULL;
        }

        return;
    }

    if (view->priv->gcs[color_num])
        return;

    window = gtk_text_view_get_window (GTK_TEXT_VIEW (view),
                                       GTK_TEXT_WINDOW_TEXT);
    g_return_if_fail (window != NULL);

    if (view->priv->colors[color_num])
    {
        if (!gdk_rgba_parse (&color, view->priv->colors[color_num]))
        {
            g_warning ("could not parse color %s",
                       view->priv->colors[color_num]);
        gtk_style_context_get_background_color(gtk_widget_get_style_context(widget), GTK_STATE_FLAG_NORMAL, &color);
        }
    }
    else
    {
        gtk_style_context_get_background_color(gtk_widget_get_style_context(widget), GTK_STATE_FLAG_NORMAL, &color);
        }

    if (!view->priv->gcs[color_num])
        view->priv->gcs[color_num] = gdk_rgba_copy(&color);
}

static void
update_gcs (MooTextView *view)
{
    update_gc (view, MOO_TEXT_VIEW_COLOR_CURRENT_LINE);
    update_gc (view, MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN);
}


static void
moo_text_view_draw_right_margin (GtkTextView    *text_view,
                                 cairo_t *cr_param)
{
    int x, y;
    GdkWindow *window = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_TEXT);
    MooTextView *view = MOO_TEXT_VIEW (text_view);

    update_right_margin (view);

    y = gdk_window_get_height (window);
    x = view->priv->right_margin_pixel_offset + gtk_text_view_get_left_margin (text_view);
    if (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (text_view)))
        x -= gtk_adjustment_get_value (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (text_view)));

    
    gdk_cairo_set_source_rgba(cr_param, view->priv->gcs[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN]);
    cairo_rectangle(cr_param, x, 0, 1, y);
    cairo_fill(cr_param);
    
}

static void
moo_text_view_get_line_yrange (GtkTextView       *text_view,
                               const GtkTextIter *iter,
                               int               *y,
                               int               *height)
{
    if (gtk_text_view_get_wrap_mode (text_view) == GTK_WRAP_NONE)
    {
        gtk_text_view_get_line_yrange (text_view, iter, y, height);
    }
    else
    {
        GdkRectangle rect;
        gtk_text_view_get_iter_location (text_view, iter, &rect);
        *y = rect.y;
        *height = rect.height;
    }
}

static void
moo_text_view_draw_current_line (GtkTextView    *text_view,
                                 cairo_t *cr_param)
{
    GdkRectangle visible_rect;
    GdkRectangle redraw_rect;
    GtkTextIter cur;
    int y, height;
    int win_y;
    int margin;
    

    gtk_text_buffer_get_iter_at_mark (gtk_text_view_get_buffer (text_view),
                                      &cur,
                                      gtk_text_buffer_get_insert (gtk_text_view_get_buffer (text_view)));

    moo_text_view_get_line_yrange (text_view, &cur, &y, &height);

    gtk_text_view_get_visible_rect (text_view, &visible_rect);

    gtk_text_view_buffer_to_window_coords (text_view,
                                           GTK_TEXT_WINDOW_TEXT,
                                           visible_rect.x,
                                           visible_rect.y,
                                           &redraw_rect.x,
                                           &redraw_rect.y);

    gtk_text_view_buffer_to_window_coords (text_view,
                                           GTK_TEXT_WINDOW_TEXT,
                                           0,
                                           y,
                                           NULL,
                                           &win_y);

    redraw_rect.width = visible_rect.width;
    redraw_rect.height = visible_rect.height;

    if (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (text_view)))
    	margin = gtk_text_view_get_left_margin (text_view) - (int) gtk_adjustment_get_value (gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (text_view)));
    else
    	margin = gtk_text_view_get_left_margin (text_view);

    
    cairo_set_source_rgb(cr_param, 0.1, 0.1, 0.1);
    gdk_cairo_set_source_rgba(cr_param, MOO_TEXT_VIEW(text_view)->priv->gcs[MOO_TEXT_VIEW_COLOR_CURRENT_LINE]);
    cairo_rectangle(cr_param, redraw_rect.x + MAX(0, margin - 1),
                        win_y,
                        redraw_rect.width,
                        height);
    cairo_fill(cr_param);
    /* GTK3: cr lifetime managed by caller */
}


static void
draw_tab_at_iter (GtkTextView    *text_view,
                  cairo_t *cr_param,
                  GtkTextIter    *iter)
{
    GdkRectangle rect;
    GdkPoint points[3];

    gtk_text_view_get_iter_location (text_view, iter, &rect);
    gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_TEXT,
                                           rect.x, rect.y + rect.height - 2,
                                           &points[0].x, &points[0].y);
    points[1] = points[0];
    points[2] = points[0];
    points[1].y += 1;
    points[2].x += 1;
    points[2].y += 1;
    /* GTK3: TODO convert gdk_draw_polygon to cairo path */
    cairo_move_to (cr_param, points[0].x, points[0].y);
    cairo_line_to (cr_param, points[1].x, points[1].y);
    cairo_line_to (cr_param, points[2].x, points[2].y);
    cairo_close_path (cr_param);
    cairo_stroke (cr_param);
}

static void
moo_text_view_draw_whitespace (GtkTextView       *text_view,
                               cairo_t *cr_param,
                               const GtkTextIter *start,
                               const GtkTextIter *end)
{
    MooTextView *view = MOO_TEXT_VIEW (text_view);
    GtkTextIter iter = *start;
    GtkTextIter cursor;
    int cursor_line;
    int line;

    if (view->priv->draw_whitespace == 0)
        return;

    line = gtk_text_iter_get_line (&iter);
    moo_text_view_get_cursor (GTK_TEXT_VIEW (text_view), &cursor);
    cursor_line = gtk_text_iter_get_line (&cursor);
    cursor_line = -1; /* FIXME */

    do
    {
        gboolean trailing = TRUE;

        if (!gtk_text_iter_ends_line (&iter))
            gtk_text_iter_forward_to_line_end (&iter);

        while (!gtk_text_iter_starts_line (&iter))
        {
            gunichar c;

            if (line == cursor_line && gtk_text_iter_compare (&iter, &cursor) <= 0)
                break;

            gtk_text_iter_backward_char (&iter);
            c = gtk_text_iter_get_char (&iter);

            if (g_unichar_isspace (c))
            {
                if ((trailing && (view->priv->draw_whitespace & MOO_DRAW_WS_TRAILING) != 0) ||
                    (c == '\t' && (view->priv->draw_whitespace & MOO_DRAW_WS_TABS) != 0) ||
                    (c != '\t' && (view->priv->draw_whitespace & MOO_DRAW_WS_SPACES) != 0))
                        draw_tab_at_iter (text_view, cr_param, &iter);
            }
            else if (trailing)
            {
                trailing = FALSE;
                if ((view->priv->draw_whitespace & ~MOO_DRAW_WS_TRAILING) == 0)
                    break;
            }
        }

        gtk_text_iter_forward_line (&iter);
        line += 1;
    }
    while (gtk_text_iter_compare (&iter, end) < 0);
}/* ------------------------------------------------------------------ */
/* Draw box (column) selection — per-character highlight                */
/* ------------------------------------------------------------------ */
static void
moo_text_view_draw_box_selection (GtkTextView *text_view, cairo_t *cr)
{
    MooTextView *view = MOO_TEXT_VIEW (text_view);
    int ax, ay, bx, by;
    int first_line, last_line, line;
    int left_bx, right_bx;
    GtkTextBuffer *buf;
    GtkStyleContext *ctx;
    GdkRGBA sel_bg;

    if (!view->priv->box_sel.active)
        return;

    ax = view->priv->box_sel.anchor_x;
    ay = view->priv->box_sel.anchor_y;
    bx = view->priv->box_sel.current_x;
    by = view->priv->box_sel.current_y;

    buf = gtk_text_view_get_buffer (text_view);

    /* Get theme selection color */
    ctx = gtk_widget_get_style_context (GTK_WIDGET (text_view));
    gtk_style_context_save (ctx);
    gtk_style_context_set_state (ctx, GTK_STATE_FLAG_SELECTED);
    gtk_style_context_get_background_color (ctx,
        gtk_style_context_get_state (ctx), &sel_bg);
    gtk_style_context_restore (ctx);

    /* Determine line range */
    {
        GtkTextIter ia, ib;
        gtk_text_view_get_iter_at_location (text_view, &ia, ax, ay);
        gtk_text_view_get_iter_at_location (text_view, &ib, bx, by);
        first_line = gtk_text_iter_get_line (&ia);
        last_line = gtk_text_iter_get_line (&ib);
        if (first_line > last_line)
        {
            int t = first_line; first_line = last_line; last_line = t;
        }
    }

    left_bx = (ax < bx) ? ax : bx;
    right_bx = (ax > bx) ? ax : bx;

    cairo_save (cr);

    for (line = first_line; line <= last_line; line++)
    {
        GtkTextIter ls, le;
        int line_len;
        int col_left, col_right;
        int ly, lh, wy;

        gtk_text_buffer_get_iter_at_line (buf, &ls, line);
        le = ls;
        if (!gtk_text_iter_ends_line (&le))
            gtk_text_iter_forward_to_line_end (&le);
        line_len = gtk_text_iter_get_line_offset (&le);

        gtk_text_view_get_line_yrange (text_view, &ls, &ly, &lh);
        gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_TEXT,
            0, ly, NULL, &wy);

        col_left  = box_sel_visual_col_at_x (text_view, line, left_bx,  FALSE);
        col_right = box_sel_visual_col_at_x (text_view, line, right_bx, TRUE);

        if (col_left >= col_right)
            continue;

        if (col_left < line_len)
        {
            GdkRectangle rl, rr;
            GtkTextIter draw_l, draw_r;
            int wx1, wx2, hx, hw, dummy;

            /* Use containing-character snap for drawing so the highlight
             * covers the full character under each drag endpoint, rather
             * than the midpoint-snapped col that can place the left edge
             * up to half a character before the actual drag position. */
            gtk_text_view_get_iter_at_location (text_view, &draw_l, left_bx, ly);
            if (gtk_text_iter_get_line (&draw_l) != line)
                draw_l = ls;

            if (col_right <= line_len)
            {
                gtk_text_view_get_iter_at_location (text_view, &draw_r, right_bx, ly);
                if (gtk_text_iter_get_line (&draw_r) != line)
                    draw_r = le;
            }
            else
            {
                /* right_bx past end of text — back up to last real char */
                draw_r = le;
                if (!gtk_text_iter_starts_line (&draw_r))
                    gtk_text_iter_backward_char (&draw_r);
            }

            gtk_text_view_get_iter_location (text_view, &draw_l, &rl);
            gtk_text_view_get_iter_location (text_view, &draw_r, &rr);
            gtk_text_view_buffer_to_window_coords (text_view,
                GTK_TEXT_WINDOW_TEXT,
                rl.x, 0, &wx1, &dummy);
            gtk_text_view_buffer_to_window_coords (text_view,
                GTK_TEXT_WINDOW_TEXT,
                rr.x + rr.width, 0, &wx2, &dummy);
            hx = wx1;
            hw = wx2 - wx1;

            if (hw > 0)
            {
                cairo_set_source_rgba (cr, sel_bg.red, sel_bg.green,
                                       sel_bg.blue, 0.45);
                cairo_rectangle (cr, hx, wy, hw, lh);
                cairo_fill (cr);
            }

            /* Virtual extension past end of line */
            if (col_right > line_len)
            {
                GdkRectangle end_rect;
                int ext_x, ext_w;
                PangoLayout *pl;
                int cw;

                gtk_text_view_get_iter_location (text_view, &le, &end_rect);
                gtk_text_view_buffer_to_window_coords (text_view,
                    GTK_TEXT_WINDOW_TEXT,
                    end_rect.x + end_rect.width, 0, &ext_x, &dummy);

                pl = gtk_widget_create_pango_layout (GTK_WIDGET (text_view), "X");
                pango_layout_get_pixel_size (pl, &cw, NULL);
                g_object_unref (pl);
                ext_w = (col_right - line_len) * cw;

                if (ext_w > 0)
                {
                    cairo_set_source_rgba (cr, sel_bg.red, sel_bg.green,
                                           sel_bg.blue, 0.20);
                    cairo_rectangle (cr, ext_x, wy, ext_w, lh);
                    cairo_fill (cr);
                }
            }
        }
        else
        {
            /* Line shorter than left bound — virtual highlight only */
            GdkRectangle end_rect;
            PangoLayout *pl;
            int cw, vx, vw, dummy;

            gtk_text_view_get_iter_location (text_view, &le, &end_rect);
            gtk_text_view_buffer_to_window_coords (text_view,
                GTK_TEXT_WINDOW_TEXT,
                end_rect.x, 0, &vx, &dummy);

            pl = gtk_widget_create_pango_layout (GTK_WIDGET (text_view), "X");
            pango_layout_get_pixel_size (pl, &cw, NULL);
            g_object_unref (pl);

            vx += col_left * cw;
            vw = (col_right - col_left) * cw;

            if (vw > 0)
            {
                cairo_set_source_rgba (cr, sel_bg.red, sel_bg.green,
                                       sel_bg.blue, 0.20);
                cairo_rectangle (cr, vx, wy, vw, lh);
                cairo_fill (cr);
            }
        }
    }

    cairo_restore (cr);
}


/* Draw "..." indicator at the end of truncated long lines            */
/* ------------------------------------------------------------------ */

#ifndef MOO_LONG_LINE_TAG
#define MOO_LONG_LINE_TAG "moo-ll-hidden"
#endif

static void
moo_text_view_draw_long_line_markers (GtkTextView *text_view,
                                      cairo_t     *cr)
{
    GtkTextBuffer *buffer;
    GtkTextTagTable *table;
    GtkTextTag *htag;
    GtkTextIter vis_start, vis_end;
    GdkRectangle visible_rect;
    int line, last_line;

    buffer = gtk_text_view_get_buffer (text_view);
    table = gtk_text_buffer_get_tag_table (buffer);
    htag = gtk_text_tag_table_lookup (table, MOO_LONG_LINE_TAG);
    if (!htag) return;

    if (!GPOINTER_TO_INT (g_object_get_data (G_OBJECT (buffer), "moo-nowrap-mode")))
        return;

    gtk_text_view_get_visible_rect (text_view, &visible_rect);
    gtk_text_view_get_line_at_y (text_view, &vis_start, visible_rect.y, NULL);
    gtk_text_view_get_line_at_y (text_view, &vis_end,
                                  visible_rect.y + visible_rect.height, NULL);
    if (!gtk_text_iter_ends_line (&vis_end))
        gtk_text_iter_forward_to_line_end (&vis_end);
    last_line = gtk_text_iter_get_line (&vis_end);

    cairo_save (cr);

    for (line = gtk_text_iter_get_line (&vis_start); line <= last_line; line++)
    {
        GtkTextIter line_start, tag_start, last_vis;
        int win_x, win_y, right_edge;
        int draw_x;
        GdkRectangle char_rect;

        gtk_text_buffer_get_iter_at_line (buffer, &line_start, line);
        tag_start = line_start;
        if (!gtk_text_iter_forward_to_tag_toggle (&tag_start, htag))
            continue;
        if (gtk_text_iter_get_line (&tag_start) != line)
            continue;
        if (gtk_text_iter_get_line_offset (&tag_start) == 0)
            continue;

        /* Get position after last visible character */
        last_vis = tag_start;
        gtk_text_iter_backward_char (&last_vis);
        gtk_text_view_get_iter_location (text_view, &last_vis, &char_rect);
        gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_TEXT,
            char_rect.x + char_rect.width, char_rect.y,
            &win_x, &win_y);

        /* LL_RIGHT_EDGE_FIX — use actual window width, not buffer coords */
        right_edge = gdk_window_get_width (
            gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_TEXT));

        {
            PangoLayout *layout;
            int tw, th;
            GdkRGBA fg = {1.0, 1.0, 1.0, 1.0};
            GdkRGBA bg = {0.33, 0.33, 0.33, 1.0};

            layout = gtk_widget_create_pango_layout (GTK_WIDGET (text_view), " ...");
            pango_layout_get_pixel_size (layout, &tw, &th);

            /* Draw at the truncation point if it's visible,
             * otherwise pin to the right edge of the viewport.
             * This ensures "..." is always visible when the line
             * is truncated, even if the truncation point is
             * beyond the current scroll position. */
            if (win_x >= 0 && win_x + tw + 4 <= right_edge)
                draw_x = win_x;
            else
                draw_x = right_edge - tw; /* flush to right edge */

            /* Only draw if draw_x is within the visible area */
            if (draw_x < 0)
                draw_x = 0;

            cairo_set_source_rgba (cr, bg.red, bg.green, bg.blue, bg.alpha);
            cairo_rectangle (cr, draw_x, win_y, right_edge - draw_x + 20, char_rect.height);
            cairo_fill (cr);

            cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, fg.alpha);
                    /* LL_MARKER_CLAMP_FIX — pin marker to right edge */
                    {
                        GdkWindow *_tw = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_TEXT);
                        if (_tw)
                        {
                            int _tw_w = gdk_window_get_width (_tw);
                            int _mk_w, _mk_h;
                            pango_layout_get_pixel_size (layout, &_mk_w, &_mk_h);
                            if (win_x + _mk_w > _tw_w || win_x > _tw_w)
                                win_x = _tw_w - _mk_w;
                        }
                    }
            cairo_move_to (cr, draw_x + 2, win_y + (char_rect.height - th) / 2);
            pango_cairo_show_layout (cr, layout);
            g_object_unref (layout);
        }
    }

    cairo_restore (cr);
}

/* Decorations that must appear underneath the text.
 *
 * GtkTextView hands ::draw_layer a cairo_t already transformed to the text
 * window and translated by the scroll offset, i.e. buffer coordinates map
 * directly.  The helpers below predate GTK3 and still work in text-window
 * coordinates, so translate by the buffer position of the window origin
 * first; that is the identity when the view is scrolled to the top. */
static void
moo_text_view_draw_layer (GtkTextView      *text_view,
                          GtkTextViewLayer  layer,
                          cairo_t          *cr)
{
    MooTextView *view = MOO_TEXT_VIEW (text_view);
    int win_x, win_y;

    if (layer != GTK_TEXT_VIEW_LAYER_BELOW_TEXT)
        return;

    if (!gtk_widget_get_sensitive (GTK_WIDGET (view)))
        return;

    gtk_text_view_window_to_buffer_coords (text_view, GTK_TEXT_WINDOW_TEXT,
                                           0, 0, &win_x, &win_y);

    cairo_save (cr);
    cairo_translate (cr, win_x, win_y);

    if ((gtk_widget_has_focus (GTK_WIDGET (view)) ||
         view->priv->highlight_current_line_unfocused)
        && view->priv->color_settings[MOO_TEXT_VIEW_COLOR_CURRENT_LINE]
        && view->priv->gcs[MOO_TEXT_VIEW_COLOR_CURRENT_LINE])
    {
        moo_text_view_draw_current_line (text_view, cr);
    }

    if (gtk_widget_has_focus (GTK_WIDGET (view)) &&
        view->priv->color_settings[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN] &&
        view->priv->gcs[MOO_TEXT_VIEW_COLOR_RIGHT_MARGIN])
    {
        moo_text_view_draw_right_margin (text_view, cr);
    }

    draw_marks_background (view, cr);

    cairo_restore (cr);
}

static gboolean
moo_text_view_expose (GtkWidget      *widget,
                      cairo_t *cr)
{
    gboolean handled;
    MooTextView *view = MOO_TEXT_VIEW (widget);
    GtkTextView *text_view = GTK_TEXT_VIEW (widget);
    GdkWindow *text_window = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_TEXT);
    GdkWindow *left_window = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_LEFT);
    GtkTextIter start, end;

    update_gcs (view);

    view->priv->in_expose = TRUE;

    if (view->priv->update_n_lines_idle)
        update_n_lines_idle (view);

    /* The current-line highlight, the right margin and the mark backgrounds
       all belong *under* the text, and are drawn from ::draw_layer -- see
       moo_text_view_draw_layer below.  Drawing them here, before chaining up,
       put them in the wrong coordinate space (this cairo_t is widget-relative,
       while they compute text-window coordinates) and GtkTextView then painted
       its own background straight over the top of them. */

    if (left_window && GDK_IS_WINDOW(left_window) && gtk_cairo_should_draw_window (cr, left_window))
        draw_left_margin (view, cr);

    if (text_window && GDK_IS_WINDOW(text_window) && gtk_cairo_should_draw_window (cr, text_window))
    {
        GdkRectangle visible_rect;

        gtk_text_view_get_visible_rect (text_view, &visible_rect);
        gtk_text_view_get_line_at_y (text_view, &start, visible_rect.y, NULL);
        gtk_text_iter_backward_line (&start);
        gtk_text_view_get_line_at_y (text_view, &end, visible_rect.y + visible_rect.height, NULL);
        gtk_text_iter_forward_line (&end);

        _moo_text_buffer_update_highlight (get_moo_buffer (view), &start, &end, FALSE);
    }

    handled = GTK_WIDGET_CLASS(moo_text_view_parent_class)->draw (widget, cr);
    /* --- FOLD VISUALS overlay (after parent draw) --- */
    if (view->priv->enable_folding)
    {
        GdkWindow *text_window =
            gtk_text_view_get_window (GTK_TEXT_VIEW (widget), GTK_TEXT_WINDOW_TEXT);

        if (text_window && gtk_cairo_should_draw_window (cr, text_window))
        {
            cairo_save (cr);
            gtk_cairo_transform_to_window (cr, widget, text_window);
            _moo_text_view_draw_fold_guides (MOO_TEXT_VIEW (widget), cr);
            _moo_text_view_draw_fold_ellipsis (MOO_TEXT_VIEW (widget), cr);
            cairo_restore (cr);
        }
    }

    /* Fill left margin background after parent draw.  Tint the editor's
     * actual text-view background (pulled from the GtkStyleContext) so
     * the gutter matches whatever theme is in effect: slightly darker
     * for light themes, slightly lighter for dark themes.  Hardcoding
     * 0.22 gray made the line numbers unreadable on Windows' default
     * (light) theme. */
    if (left_window && GDK_IS_WINDOW(left_window) && gtk_cairo_should_draw_window(cr, left_window))
    {
        int lw_w = gdk_window_get_width(left_window);
        int lw_h = gdk_window_get_height(left_window);
        GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
        GdkRGBA bg = {0.94, 0.94, 0.94, 1.0};
        double luma, delta;

        gtk_style_context_save(ctx);
        gtk_style_context_add_class(ctx, GTK_STYLE_CLASS_VIEW);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_style_context_get_background_color(ctx,
                gtk_style_context_get_state(ctx), &bg);
        G_GNUC_END_IGNORE_DEPRECATIONS
        gtk_style_context_restore(ctx);

        /* Some themes report a fully-transparent view background; treat
         * that as "use a sensible light gray default". */
        if (bg.alpha < 0.1) {
            bg.red = bg.green = bg.blue = 0.94;
            bg.alpha = 1.0;
        }

        /* Tint slightly: darken light backgrounds, lighten dark ones. */
        luma  = 0.299 * bg.red + 0.587 * bg.green + 0.114 * bg.blue;
        delta = (luma > 0.5) ? -0.06 : +0.08;
        bg.red   = CLAMP(bg.red   + delta, 0.0, 1.0);
        bg.green = CLAMP(bg.green + delta, 0.0, 1.0);
        bg.blue  = CLAMP(bg.blue  + delta, 0.0, 1.0);

        cairo_save(cr);
        gtk_cairo_transform_to_window(cr, widget, left_window);
        cairo_rectangle(cr, 0, 0, lw_w, lw_h);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, bg.red, bg.green, bg.blue, 1.0);
        cairo_paint(cr);
        cairo_restore(cr);
        draw_left_margin(view, cr);
    }

    if (text_window && GDK_IS_WINDOW(text_window) && gtk_cairo_should_draw_window (cr, text_window))
    {
        int first_line = gtk_text_iter_get_line (&start);
        int last_line = gtk_text_iter_get_line (&end);

        if (last_line - first_line < 1000)
        {
            if (view->priv->draw_whitespace != 0)
                moo_text_view_draw_whitespace (text_view, cr, &start, &end);

    {
        cairo_save (cr);
        gtk_cairo_transform_to_window (cr, widget, text_window);
        /* Draw "..." on truncated long lines */
        moo_text_view_draw_long_line_markers (text_view, cr);
        /* Draw box/column selection overlay */
        moo_text_view_draw_box_selection (text_view, cr);
        cairo_restore (cr);
    }

        }
    }

    view->priv->in_expose = FALSE;

    return handled;
}


static gboolean
invalidate_rectangle (MooTextView *view)
{
    GdkWindow *window;
    GdkRectangle *rect = view->priv->update_rectangle;

    view->priv->update_rectangle = NULL;
    view->priv->update_idle = 0;

    gtk_text_view_buffer_to_window_coords (GTK_TEXT_VIEW (view),
                                           GTK_TEXT_WINDOW_TEXT,
                                           rect->x, rect->y,
                                           &rect->x, &rect->y);
    window = gtk_text_view_get_window (GTK_TEXT_VIEW (view), GTK_TEXT_WINDOW_TEXT);
    gdk_window_invalidate_rect (window, rect, FALSE);

    g_free (rect);
    return FALSE;
}


static void
highlight_updated (GtkTextView       *text_view,
                   const GtkTextIter *start,
                   const GtkTextIter *end)
{
    GdkRectangle visible, changed, update;
    int y, height;
    MooTextView *view = MOO_TEXT_VIEW (text_view);

    if (!gtk_widget_is_drawable (GTK_WIDGET (text_view)))
        return;

    gtk_text_view_get_visible_rect (text_view, &visible);

    gtk_text_view_get_line_yrange (text_view, start, &changed.y, &height);
    gtk_text_view_get_line_yrange (text_view, end, &y, &height);
    changed.height = y - changed.y + height;
    changed.x = visible.x;
    changed.width = visible.width;

    if (!gdk_rectangle_intersect (&changed, &visible, &update))
        return;

    if (view->priv->update_rectangle)
    {
        gdk_rectangle_union (view->priv->update_rectangle, &update,
                             view->priv->update_rectangle);
    }
    else
    {
        view->priv->update_rectangle = g_new (GdkRectangle, 1);
        *view->priv->update_rectangle = update;
    }

    if (view->priv->in_expose)
    {
        g_critical ("oops");

        if (!view->priv->update_idle)
            view->priv->update_idle =
                g_idle_add_full (G_PRIORITY_HIGH_IDLE,
                                               (GSourceFunc) invalidate_rectangle,
                                               view, NULL);
    }
    else
    {
        if (view->priv->update_idle)
            g_source_remove (view->priv->update_idle);
        view->priv->update_idle = 0;

        invalidate_rectangle (view);
    }
}


static void
set_draw_whitespace (MooTextView    *view,
                     MooDrawWsFlags  flags)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (view->priv->draw_whitespace == flags)
        return;

    view->priv->draw_whitespace = flags;
    g_object_notify (G_OBJECT (view), "draw-whitespace");

    if (gtk_widget_is_drawable (GTK_WIDGET (view)))
        gtk_widget_queue_draw (GTK_WIDGET (view));
}


GtkTextTag *
moo_text_view_lookup_tag (MooTextView    *view,
                          const char     *name)
{
    GtkTextBuffer *buffer;
    GtkTextTagTable *table;

    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), NULL);
    g_return_val_if_fail (name != NULL, NULL);

    buffer = get_buffer (view);
    table = gtk_text_buffer_get_tag_table (buffer);

    return gtk_text_tag_table_lookup (table, name);
}


#if 0
MOO_DEFINE_QUARK_STATIC ("moo-text-tag-cursor", cursor_quark)

void
moo_text_tag_set_cursor (GtkTextTag    *tag,
                         GdkCursorType  cursor)
{
    g_return_if_fail (GTK_IS_TEXT_TAG (tag));
    g_object_set_qdata (G_OBJECT (tag), cursor_quark (), GINT_TO_POINTER (cursor));
}

GdkCursorType
moo_text_tag_get_cursor (GtkTextTag *tag)
{
    g_return_val_if_fail (GTK_IS_TEXT_TAG (tag), 0);
    return GPOINTER_TO_INT (g_object_get_qdata (G_OBJECT (tag), cursor_quark ()));
}
#endif


void
moo_text_view_set_lang (MooTextView    *view,
                        MooLang        *lang)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    moo_text_buffer_set_lang (get_moo_buffer (view), lang);
    moo_fold_schedule_update (view);
    gtk_widget_queue_draw (GTK_WIDGET (view));
}


/**
 * moo_text_view_set_lang_by_id: (moo.private 1)
 *
 * @view:
 * @lang_id: (type const-utf8)
 */
void
moo_text_view_set_lang_by_id (MooTextView *view,
                              const char  *lang_id)
{
    MooLangMgr *mgr;
    MooLang *lang;
    MooTextStyleScheme *scheme;

    mgr = moo_lang_mgr_default ();
    lang = moo_lang_mgr_get_lang (mgr, lang_id);
    scheme = moo_lang_mgr_get_active_scheme (mgr);

    moo_text_view_set_style_scheme (view, scheme);
    moo_text_view_set_lang (view, lang);
}


MooLang*
moo_text_view_get_lang (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), NULL);
    return moo_text_buffer_get_lang (get_moo_buffer (view));
}


static void
moo_text_view_apply_style_scheme (MooTextView        *view,
                                  MooTextStyleScheme *scheme)
{
    _moo_text_style_scheme_apply (scheme, GTK_WIDGET (view));
    _moo_text_buffer_set_style_scheme (get_moo_buffer (view), scheme);
}

void
moo_text_view_set_style_scheme (MooTextView        *view,
                                MooTextStyleScheme *scheme)
{
    g_return_if_fail (MOO_IS_TEXT_STYLE_SCHEME (scheme));
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (view->priv->style_scheme == scheme)
        return;

    if (view->priv->style_scheme)
        g_object_unref (view->priv->style_scheme);

    view->priv->style_scheme = g_object_ref (scheme);

    MOO_TEXT_VIEW_GET_CLASS (view)->apply_style_scheme (view, scheme);
}

MooTextStyleScheme *
moo_text_view_get_style_scheme (MooTextView *view)
{
    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), NULL);
    return view->priv->style_scheme;
}


static void
moo_text_view_populate_popup (GtkTextView    *text_view,
                              GtkMenu        *menu)
{
    MooTextView *view = MOO_TEXT_VIEW (text_view);
    GtkWidget *item;

    item = gtk_separator_menu_item_new ();
    gtk_widget_show (item);
    gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);

    item = gtk_menu_item_new_with_label ("Redo");
    gtk_widget_show (item);
    gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
    g_signal_connect_swapped (item, "activate", G_CALLBACK (moo_text_view_redo), view);
    gtk_widget_set_sensitive (item, moo_text_view_can_redo (view));

    item = gtk_menu_item_new_with_label ("Undo");
    gtk_widget_show (item);
    gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), item);
    g_signal_connect_swapped (item, "activate", G_CALLBACK (moo_text_view_undo), view);
    gtk_widget_set_sensitive (item, moo_text_view_can_undo (view));
}


int
_moo_text_view_get_line_height (MooTextView *view)
{
    PangoContext *ctx;
    PangoLayout *layout;
    PangoRectangle rect;
    int height;

    g_return_val_if_fail (MOO_IS_TEXT_VIEW (view), 10);

    ctx = gtk_widget_get_pango_context (GTK_WIDGET (view));
    g_return_val_if_fail (ctx != NULL, 10);

    layout = pango_layout_new (ctx);
    pango_layout_set_text (layout, "AA", -1);

    pango_layout_get_extents (layout, NULL, &rect);
    height = rect.height / PANGO_SCALE;

    g_object_unref (layout);
    return height;
}


/*****************************************************************************/
/* Left margin
 */

#define MARK_ICON_LPAD      0
#define MARK_ICON_RPAD      0
#define LINE_NUMBER_LPAD    0
#define LINE_NUMBER_RPAD    3

static PangoLayout *
create_line_numbers_layout (MooTextView *view)
{
    PangoLayout *layout;

    layout = gtk_widget_create_pango_layout (GTK_WIDGET (view), NULL);

    if (view->priv->lm.numbers_font)
        pango_layout_set_font_description (layout, view->priv->lm.numbers_font);

    return layout;
}


static int
get_n_digits (MooTextView *view)
{
    int i, n;
    int lines;

    lines = MAX (99, gtk_text_buffer_get_line_count (get_buffer (view)));

    for (i = 2, n = 100; ; ++i, n *= 10)
        if (lines < n)
            return i;
}


static void
update_line_mark_icons (MooTextView *view)
{
    int i;
    char str[32];
    GtkSettings *settings;
    PangoLayout *layout;

    g_return_if_fail (gtk_widget_get_realized (GTK_WIDGET (view)));

    settings = gtk_widget_get_settings (GTK_WIDGET (view));

    if (!gtk_icon_size_lookup_for_settings (settings, GTK_ICON_SIZE_MENU,
                                            &view->priv->lm.icon_width, NULL))
        view->priv->lm.icon_width = 16;

    layout = gtk_widget_create_pango_layout (GTK_WIDGET (view), NULL);

    for (i = 1; i < 10; ++i)
    {
        PangoRectangle rect;
        g_snprintf (str, sizeof str, "<b>%d</b>", i);
        pango_layout_set_markup (layout, str, -1);
        pango_layout_get_pixel_extents  (layout, &rect, NULL);
        view->priv->lm.icon_width = MAX (view->priv->lm.icon_width, rect.width);
    }

    g_object_unref (layout);
}

static void
update_digit_width (MooTextView *view)
{
    int i;
    char str[32];
    PangoLayout *layout;

    layout = create_line_numbers_layout (view);
    view->priv->lm.digit_width = 0;
    view->priv->lm.numbers_width = 0;

    for (i = 0; i < 10; ++i)
    {
        int width;
        g_snprintf (str, sizeof str, "%d", i);
        pango_layout_set_text (layout, str, -1);
        pango_layout_get_pixel_size (layout, &width, NULL);
        view->priv->lm.digit_width = MAX (view->priv->lm.digit_width, width);
    }

    g_object_unref (layout);
}

static void
update_fold_width (MooTextView *view)
{
    gtk_widget_style_get (GTK_WIDGET (view), "expander-size",
                          &view->priv->lm.fold_width, NULL);
    view->priv->lm.fold_width += 2*EXPANDER_PAD;
}

static void
update_left_margin (MooTextView *view)
{
    int margin_size;
    int old_size;
    GtkTextView *text_view = GTK_TEXT_VIEW (view);

    if (!gtk_widget_get_realized (GTK_WIDGET (view)))
        return;

    margin_size = 0;

    if (view->priv->lm.show_icons)
    {
        update_line_mark_icons (view);
        margin_size += view->priv->lm.icon_width + MARK_ICON_LPAD + MARK_ICON_RPAD;
    }

    if (view->priv->lm.show_numbers)
    {
        update_digit_width (view);
        view->priv->lm.numbers_width = view->priv->lm.digit_width * get_n_digits (view);
        margin_size += view->priv->lm.numbers_width + LINE_NUMBER_LPAD + LINE_NUMBER_RPAD;
    }

    if (view->priv->lm.show_folds)
    {
        update_fold_width (view);
        margin_size += view->priv->lm.fold_width;
    }

    old_size = get_border_window_size (text_view, GTK_TEXT_WINDOW_LEFT);
    gtk_text_view_set_border_window_size (text_view,
                                          GTK_TEXT_WINDOW_LEFT,
                                          margin_size);

    if (old_size == 0 && margin_size != 0)
    {
        GdkWindow *window = gtk_text_view_get_window (text_view, GTK_TEXT_WINDOW_LEFT);
        GdkCursor *cursor = gdk_cursor_new_for_display (gdk_display_get_default (), GDK_RIGHT_PTR);
        gdk_window_set_cursor (window, cursor);
        g_object_unref (cursor);
    }

    /* XXX do not invalidate whole widget */
    gtk_widget_queue_draw (GTK_WIDGET (view));
}


static gboolean
update_n_lines_idle (MooTextView *view)
{
    int n_lines;

    view->priv->update_n_lines_idle = 0;

    n_lines = gtk_text_buffer_get_line_count (get_buffer (view));

    if (n_lines != view->priv->n_lines)
    {
        view->priv->n_lines = n_lines;
        update_left_margin (view);
    }

    return FALSE;
}

static void
buffer_changed (MooTextView *view)
{
    if (gtk_widget_get_realized (GTK_WIDGET (view)) &&
        view->priv->lm.show_numbers &&
        !view->priv->update_n_lines_idle)
    {
        view->priv->update_n_lines_idle =
            g_idle_add_full (G_PRIORITY_HIGH,
                                       (GSourceFunc) update_n_lines_idle,
                                       view, NULL);
    }
}


static void
draw_marks (MooTextView    *view,
            cairo_t *cr_param,
            GSList         *marks,
            int             line_y,
            int             line_height)
{
    g_return_if_fail (marks != NULL);

    while (marks)
    {
        MooLineMark *mark;
        GdkPixbuf *pixbuf;
        const char *markup;

        mark = marks->data;
        marks = marks->next;

        if (!_moo_line_mark_get_pretty (mark))
            continue;

        pixbuf = moo_line_mark_get_pixbuf (mark);

        if (pixbuf)
        {
            int pw, ph;
            int x, y;

            pw = gdk_pixbuf_get_width (pixbuf);
            ph = gdk_pixbuf_get_height (pixbuf);
            x = MARK_ICON_LPAD + (view->priv->lm.icon_width - pw) / 2;
            y = line_y + (line_height - ph) / 2;

            gdk_cairo_set_source_pixbuf (cr_param, pixbuf, x, y);
            cairo_paint (cr_param);
        }

        markup = moo_line_mark_get_markup (mark);

        if (markup)
        {
            PangoLayout *layout;
            PangoRectangle rect;
            int x, y;

            layout = gtk_widget_create_pango_layout (GTK_WIDGET (view), NULL);
            pango_layout_set_markup (layout, markup, -1);
            pango_layout_get_pixel_extents (layout, &rect, NULL);
            x = MARK_ICON_LPAD + (view->priv->lm.icon_width - rect.width) / 2;
            y = line_y;

            cairo_move_to (cr_param, x, y);
            pango_cairo_show_layout (cr_param, layout);

            g_object_unref (layout);
        }
    }
}

static void
draw_fold_mark (MooTextView    *view,
                cairo_t *cr_param,
                MooFold        *fold,
                int             y,
                int             height,
                int             window_width)
{
    /* Draw a chevron — ❯ pointing right when collapsed, ❮-rotated /
     * ⌄ pointing down when expanded.  Two stroked line segments meeting
     * at the apex, instead of a filled triangle: lighter visual weight,
     * still 100 % cairo (no font dependency, so no tofu on Windows). */
    int cx, cy;
    GtkStyleContext *ctx;
    GdkRGBA fg = {0.6, 0.6, 0.6, 1.0};
    double half, lw;
    int target_px;

    cx = window_width - view->priv->lm.fold_width / 2;
    cy = y + height / 2;

    /* Foreground color from theme, slightly dimmed */
    ctx = gtk_widget_get_style_context (GTK_WIDGET (view));
    gtk_style_context_save (ctx);
    gtk_style_context_get_color (ctx, gtk_style_context_get_state (ctx), &fg);
    gtk_style_context_restore (ctx);
    fg.alpha = 0.75;

    target_px = view->priv->lm.fold_width - 4;
    if (target_px < 6) target_px = 6;
    half = target_px / 2.0;
    /* Stroke width scales with size: ~1/6 of the glyph height, clamped
     * so it doesn't disappear on small sizes or get clunky on big ones. */
    lw = half / 3.0;
    if (lw < 1.2) lw = 1.2;
    if (lw > 2.4) lw = 2.4;

    cairo_save (cr_param);
    cairo_set_source_rgba (cr_param, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_set_line_width (cr_param, lw);
    cairo_set_line_cap  (cr_param, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr_param, CAIRO_LINE_JOIN_ROUND);
    cairo_translate (cr_param, cx, cy);

    if (fold->collapsed)
    {
        /* ❯ — apex on the right, opens leftward */
        cairo_move_to (cr_param, -half * 0.45, -half * 0.7);
        cairo_line_to (cr_param,  half * 0.45,           0);
        cairo_line_to (cr_param, -half * 0.45,  half * 0.7);
    }
    else
    {
        /* ⌄ — apex at the bottom, opens upward */
        cairo_move_to (cr_param, -half * 0.7, -half * 0.45);
        cairo_line_to (cr_param,           0,  half * 0.45);
        cairo_line_to (cr_param,  half * 0.7, -half * 0.45);
    }

    cairo_stroke (cr_param);
    cairo_restore (cr_param);
}

static void
draw_left_margin (MooTextView    *view,
                  cairo_t *cr_param)
{
    GtkTextView *text_view;
    GtkTextBuffer *buffer;
    PangoLayout *layout = NULL;
    int line, current_line, text_width, window_width, mark_icon_width;
    GdkRectangle area;
    GtkTextIter iter;
    char str[32];

    text_view = GTK_TEXT_VIEW (view);
    buffer = gtk_text_view_get_buffer (text_view);
    window_width = SAFE_GDK_WINDOW_GET_WIDTH (gtk_text_view_get_window (GTK_TEXT_VIEW (view), GTK_TEXT_WINDOW_LEFT));
    text_width = 0;
    mark_icon_width = 0;

    if (view->priv->lm.show_numbers)
    {
        layout = create_line_numbers_layout (view);
        text_width = view->priv->lm.numbers_width;
    }

    if (view->priv->lm.show_icons)
        mark_icon_width = view->priv->lm.icon_width + MARK_ICON_LPAD + MARK_ICON_RPAD;

    gtk_text_buffer_get_iter_at_mark (buffer, &iter, gtk_text_buffer_get_insert (buffer));
    current_line = gtk_text_iter_get_line (&iter);

    gdk_cairo_get_clip_rectangle (cr_param, &area);
    gtk_text_view_window_to_buffer_coords (text_view,
                                           GTK_TEXT_WINDOW_LEFT,
                                           area.x, area.y,
                                           &area.x, &area.y);

    gtk_text_view_get_line_at_y (text_view, &iter, area.y, NULL);
    line = gtk_text_iter_get_line (&iter);

    while (TRUE)
    {
        int y, height;

        gtk_text_view_get_line_yrange (text_view, &iter, &y, &height);

        if (y > area.y + area.height)
            break;

        gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_LEFT,
                                               0, y, NULL, &y);

        if (view->priv->lm.show_numbers)
        {
            int x, w;

            if (line == current_line)
                g_snprintf (str, sizeof str, "<b>%d</b>", line + 1);
            else
                g_snprintf (str, sizeof str, "%d", line + 1);

            pango_layout_set_markup (layout, str, -1);
            pango_layout_get_pixel_size (layout, &w, NULL);
            x = mark_icon_width + LINE_NUMBER_LPAD + text_width - w;

            {
                GtkStyleContext *context =
                    gtk_widget_get_style_context (GTK_WIDGET (view));
                gtk_style_context_save (context);
                gtk_style_context_set_state (context,
                    gtk_widget_get_state_flags (GTK_WIDGET (view)));
                gtk_render_layout (context, cr_param, x, y, layout);
                gtk_style_context_restore (context);
            }
        }

        if (view->priv->lm.show_folds)
        {
            MooFold *fold = moo_text_buffer_get_fold_at_line (get_moo_buffer (view), line);

            if (fold)
                draw_fold_mark (view, cr_param, fold, y, height, window_width);
        }

        if (view->priv->lm.show_icons)
        {
            GSList *marks = moo_text_buffer_get_line_marks_at_line (get_moo_buffer (view), line);

            if (marks)
                draw_marks (view, cr_param, marks, y, height);

            g_slist_free (marks);
        }

        if (!text_iter_forward_visible_line (view, &iter, &line))
            break;
    }

    if (layout)
        g_object_unref (layout);
}


static void
draw_fold_background (MooTextView    *view,
                      cairo_t *cr_param,
                      MooFold        *fold,
                      int             y,
                      int             height,
                      int             window_width)
{
    if (!fold->collapsed)
        return;

    /* Override window_width: use text area width, not left margin */
    window_width = SAFE_GDK_WINDOW_GET_WIDTH (gtk_text_view_get_window (GTK_TEXT_VIEW (view), GTK_TEXT_WINDOW_TEXT));

    /* 1. Shade the fold header line background */
    {
        GtkStyleContext *ctx;
        GdkRGBA bg;
        int left_margin = gtk_text_view_get_left_margin (GTK_TEXT_VIEW (view));

        ctx = gtk_widget_get_style_context (GTK_WIDGET (view));
        gtk_style_context_save (ctx);
        gtk_style_context_get_background_color (ctx,
            gtk_style_context_get_state (ctx), &bg);
        gtk_style_context_restore (ctx);

        /* Slightly darken/lighten the background for the fold header */
        if (bg.red + bg.green + bg.blue > 1.5)
        {
            /* Light theme — darken slightly */
            bg.red   = MAX (0.0, bg.red   - 0.04);
            bg.green = MAX (0.0, bg.green - 0.04);
            bg.blue  = MAX (0.0, bg.blue  - 0.04);
        }
        else
        {
            /* Dark theme — lighten slightly */
            bg.red   = MIN (1.0, bg.red   + 0.06);
            bg.green = MIN (1.0, bg.green + 0.06);
            bg.blue  = MIN (1.0, bg.blue  + 0.06);
        }

        cairo_set_source_rgba (cr_param, bg.red, bg.green, bg.blue, 1.0);
        cairo_rectangle (cr_param, left_margin, y,
                         window_width, height);
        cairo_fill (cr_param);
    }

    /* 2. Draw bottom border line */
    {
        GtkStyleContext *ctx;
        GdkRGBA fg;
        int left_margin = gtk_text_view_get_left_margin (GTK_TEXT_VIEW (view));

        ctx = gtk_widget_get_style_context (GTK_WIDGET (view));
        gtk_style_context_save (ctx);
        gtk_style_context_get_color (ctx,
            gtk_style_context_get_state (ctx), &fg);
        gtk_style_context_restore (ctx);
        fg.alpha = 0.15;

        cairo_set_source_rgba (cr_param, fg.red, fg.green, fg.blue, fg.alpha);
        cairo_set_line_width (cr_param, 1.0);
        cairo_move_to (cr_param, left_margin, y + height - 0.5);
        cairo_line_to (cr_param, left_margin + window_width, y + height - 0.5);
        cairo_stroke (cr_param);
    }

    /* 3. Draw "..." marker after the fold header text */
    {
        GtkTextView *text_view = GTK_TEXT_VIEW (view);
        GtkTextBuffer *buffer = gtk_text_view_get_buffer (text_view);
        GtkTextIter line_end;
        GdkRectangle end_rect;
        int fold_line = _moo_fold_get_start (fold);
        int wx, wy;

        gtk_text_buffer_get_iter_at_line (buffer, &line_end, fold_line);
        if (!gtk_text_iter_ends_line (&line_end))
            gtk_text_iter_forward_to_line_end (&line_end);

        gtk_text_view_get_iter_location (text_view, &line_end, &end_rect);
        gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_TEXT,
            end_rect.x + end_rect.width, end_rect.y, &wx, &wy);

        {
            PangoLayout *layout;
            int tw, th;
            GtkStyleContext *ctx;
            GdkRGBA marker_fg = {0.5, 0.5, 0.5, 0.8};
            GdkRGBA marker_bg = {0.0, 0.0, 0.0, 0.0};

            ctx = gtk_widget_get_style_context (GTK_WIDGET (view));
            gtk_style_context_save (ctx);
            gtk_style_context_get_color (ctx,
                gtk_style_context_get_state (ctx), &marker_fg);
            gtk_style_context_get_background_color (ctx,
                gtk_style_context_get_state (ctx), &marker_bg);
            gtk_style_context_restore (ctx);

            /* Adjust marker colors */
            marker_fg.alpha = 0.6;

            /* Slightly different background for the marker box */
            if (marker_bg.red + marker_bg.green + marker_bg.blue > 1.5)
            {
                marker_bg.red   = MAX (0.0, marker_bg.red   - 0.08);
                marker_bg.green = MAX (0.0, marker_bg.green - 0.08);
                marker_bg.blue  = MAX (0.0, marker_bg.blue  - 0.08);
            }
            else
            {
                marker_bg.red   = MIN (1.0, marker_bg.red   + 0.10);
                marker_bg.green = MIN (1.0, marker_bg.green + 0.10);
                marker_bg.blue  = MIN (1.0, marker_bg.blue  + 0.10);
            }
            marker_bg.alpha = 1.0;

            layout = gtk_widget_create_pango_layout (GTK_WIDGET (view), " \xe2\x80\xa6 ");
            pango_layout_get_pixel_size (layout, &tw, &th);

            /* Draw marker background */
            cairo_set_source_rgba (cr_param,
                marker_bg.red, marker_bg.green, marker_bg.blue, marker_bg.alpha);

            {
                double rx = wx + 4;
                double ry = y + (height - th) / 2.0 - 1;
                double rw = tw + 2;
                double rh = th + 2;
                double radius = 3.0;

                /* Rounded rectangle */
                cairo_new_sub_path (cr_param);
                cairo_arc (cr_param, rx + rw - radius, ry + radius, radius, -G_PI/2, 0);
                cairo_arc (cr_param, rx + rw - radius, ry + rh - radius, radius, 0, G_PI/2);
                cairo_arc (cr_param, rx + radius, ry + rh - radius, radius, G_PI/2, G_PI);
                cairo_arc (cr_param, rx + radius, ry + radius, radius, G_PI, 3*G_PI/2);
                cairo_close_path (cr_param);
                cairo_fill (cr_param);
            }

            /* Draw "..." text */
            cairo_set_source_rgba (cr_param,
                marker_fg.red, marker_fg.green, marker_fg.blue, marker_fg.alpha);
            cairo_move_to (cr_param, wx + 5, y + (height - th) / 2.0);
            pango_cairo_show_layout (cr_param, layout);

            g_object_unref (layout);
        }
    }
}

static void
draw_marks_background (MooTextView    *view,
                       cairo_t *cr_param)
{
    GtkTextView *text_view;
    int line, window_width;
    GdkRectangle area;
    GtkTextIter iter;
    cairo_t* cr = NULL;

    text_view = GTK_TEXT_VIEW (view);

    gdk_cairo_get_clip_rectangle (cr_param, &area);
    gtk_text_view_window_to_buffer_coords (text_view,
                                           GTK_TEXT_WINDOW_TEXT,
                                           area.x, area.y,
                                           &area.x, &area.y);

    window_width = SAFE_GDK_WINDOW_GET_WIDTH (gtk_text_view_get_window (GTK_TEXT_VIEW (view), GTK_TEXT_WINDOW_LEFT));
    window_width -= gtk_text_view_get_left_margin (text_view);

    gtk_text_view_get_line_at_y (text_view, &iter, area.y, NULL);
    line = gtk_text_iter_get_line (&iter);

    while (TRUE)
    {
        int y, height;

        gtk_text_view_get_line_yrange (text_view, &iter, &y, &height);

        if (y > area.y + area.height)
            break;

        gtk_text_view_buffer_to_window_coords (text_view, GTK_TEXT_WINDOW_TEXT,
                                               0, y, NULL, &y);

        if (view->priv->enable_folding)
        {
            MooFold *fold = moo_text_buffer_get_fold_at_line (get_moo_buffer (view), line);

            if (fold)
                draw_fold_background (view, cr_param, fold, y, height, window_width);
        }

        if (TRUE)
        {
            MooLineMark *mark;
            const GdkRGBA *color = NULL;
            GSList *marks = moo_text_buffer_get_line_marks_at_line (get_moo_buffer (view), line);

            if (marks)
            {
                while (marks)
                {
                    mark = marks->data;

                    if (_moo_line_mark_get_pretty (mark))
                        color = moo_line_mark_get_background (mark);

                    if (!color)
                        marks = g_slist_delete_link (marks, marks);
                    else
                        break;
                }

                /* XXX compose colors */
                if (color)
                {
                    if (!cr)
                        cr = cr_param; /* GTK3: use passed-in cairo context */

                    gdk_cairo_set_source_rgba (cr, color);
                    cairo_rectangle(cr_param, gtk_text_view_get_left_margin(text_view),
                                        y,
                                        window_width,
                                        height);
                    cairo_fill (cr);
                }
            }

            g_slist_free (marks);
        }

        if (!text_iter_forward_visible_line (view, &iter, &line))
            break;
    }

    /* GTK3: cairo managed by caller */
    /* GTK3: cr lifetime managed by caller */
}


void
moo_text_view_set_show_line_numbers (MooTextView *view,
                                     gboolean     show)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    show = show != 0;

    if (view->priv->lm.show_numbers != show)
    {
        view->priv->lm.show_numbers = show;
        update_left_margin (view);
        g_object_notify (G_OBJECT (view), "show-line-numbers");
    }
}


void
_moo_text_view_set_line_numbers_font (MooTextView *view,
                                      const char  *name)
{
    PangoFontDescription *font;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (name)
        font = pango_font_description_from_string (name);
    else
        font = NULL;

    if (font == view->priv->lm.numbers_font ||
        (font != NULL && view->priv->lm.numbers_font != NULL &&
         pango_font_description_equal (font, view->priv->lm.numbers_font)))
    {
        pango_font_description_free (font);
        return;
    }

    if (view->priv->lm.numbers_font)
        pango_font_description_free (view->priv->lm.numbers_font);

    view->priv->lm.numbers_font = font;

    update_left_margin (view);
}


static void
set_show_line_marks (MooTextView *view,
                     gboolean     show)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    show = show != 0;

    if (view->priv->lm.show_icons != show)
    {
        view->priv->lm.show_icons = show;
        update_left_margin (view);
        g_object_notify (G_OBJECT (view), "show-line-marks");
    }
}


static void
moo_text_view_style_set (GtkWidget *widget,
                         GtkStyle  *prev_style)
{
    MooTextView *view = MOO_TEXT_VIEW (widget);

    invalidate_gcs (view);
    invalidate_right_margin (view);
    update_tab_width (view);
    update_left_margin (view);

    GTK_WIDGET_CLASS(moo_text_view_parent_class)->style_set (widget, prev_style);
}


static void
set_enable_folding (MooTextView *view,
                    gboolean     show)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    show = show != 0;

    if (show == view->priv->enable_folding)
        return;

    if (show && !moo_text_view_lookup_tag (view, MOO_FOLD_TAG))
        gtk_text_buffer_create_tag (get_buffer (view), MOO_FOLD_TAG,
                                    "invisible", TRUE, NULL);

    view->priv->lm.show_folds = show;
    view->priv->enable_folding = show;

    update_left_margin (view);

    g_object_notify (G_OBJECT (view), "enable-folding");
}


static void
update_tab_width (MooTextView *view)
{
    PangoTabArray *tabs;
    PangoLayout *layout;
    int tab_width;
    char *string;

    if (!gtk_widget_get_realized (GTK_WIDGET (view)))
        return;

    g_return_if_fail (view->priv->tab_width > 0);
    g_return_if_fail (gtk_widget_get_style (GTK_WIDGET (view)) != NULL);

    string = g_strnfill (view->priv->tab_width, ' ');
    layout = gtk_widget_create_pango_layout (GTK_WIDGET (view), string);
    pango_layout_get_size (layout, &tab_width, NULL);

    tabs = pango_tab_array_new (2, FALSE);
    pango_tab_array_set_tab (tabs, 0, PANGO_TAB_LEFT, 0);
    pango_tab_array_set_tab (tabs, 1, PANGO_TAB_LEFT, tab_width);

    gtk_text_view_set_tabs (GTK_TEXT_VIEW (view), tabs);

    pango_tab_array_free (tabs);
    g_object_unref (layout);
    g_free (string);
}

void
moo_text_view_set_tab_width (MooTextView *view,
                             guint        width)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (width > 0);

    if (width == view->priv->tab_width)
        return;

    view->priv->tab_width = width;
    update_tab_width (view);

    g_object_notify (G_OBJECT (view), "tab-width");
}


static void
invalidate_right_margin (MooTextView *view)
{
    view->priv->right_margin_pixel_offset = -1;
}

static void
update_right_margin (MooTextView *view)
{
    PangoLayout *layout;
    char *string;

    if (view->priv->right_margin_pixel_offset > 0 ||
        !gtk_widget_get_realized (GTK_WIDGET (view)))
            return;

    g_return_if_fail (view->priv->right_margin_offset > 0 &&
                      view->priv->right_margin_offset < 1000);

    string = g_strnfill (view->priv->right_margin_offset, '_');
    layout = gtk_widget_create_pango_layout (GTK_WIDGET (view), string);
    pango_layout_get_pixel_size (layout, &view->priv->right_margin_pixel_offset, NULL);

    g_object_unref (layout);
    g_free (string);
}


static gboolean
text_iter_forward_visible_line (MooTextView *view,
                                GtkTextIter *iter,
                                int         *line)
{
    if (!view->priv->enable_folding)
    {
        if (!gtk_text_iter_forward_line (iter))
        {
            if (gtk_text_iter_get_line (iter) == *line)
                return FALSE;
        }

        *line += 1;
        return TRUE;
    }
    else
    {
        GtkTextTagTable *table = gtk_text_buffer_get_tag_table (get_buffer (view));
        GtkTextTag *tag = gtk_text_tag_table_lookup (table, MOO_FOLD_TAG);

        g_return_val_if_fail (tag != NULL, FALSE);

        while (TRUE)
        {
            if (!gtk_text_iter_forward_line (iter))
            {
                if (gtk_text_iter_get_line (iter) == *line)
                    return FALSE;
            }

            *line += 1;

            if (!gtk_text_iter_has_tag (iter, tag) && !gtk_text_iter_begins_tag (iter, tag))
                return TRUE;
        }
    }
}


static void
invalidate_line (MooTextView *view,
                 int          line,
                 gboolean     left,
                 gboolean     text)
{
    GtkTextIter iter;
    GdkRectangle rect = {0, 0, 0, 0};

    if (!gtk_widget_is_drawable (GTK_WIDGET (view)))
        return;

    gtk_text_buffer_get_iter_at_line (get_buffer (view), &iter, line);
    gtk_text_view_get_line_yrange (GTK_TEXT_VIEW (view), &iter, &rect.y, &rect.height);

    gtk_text_view_buffer_to_window_coords (GTK_TEXT_VIEW (view),
                                           GTK_TEXT_WINDOW_TEXT,
                                           0, rect.y, NULL, &rect.y);

    if (left)
    {
        GdkWindow *window = gtk_text_view_get_window (GTK_TEXT_VIEW (view),
                                                      GTK_TEXT_WINDOW_LEFT);
        if (window)
        {
            rect.width = SAFE_GDK_WINDOW_GET_WIDTH (window);
            gdk_window_invalidate_rect (window, &rect, FALSE);
        }
    }

    if (text)
    {
        GdkWindow *window = gtk_text_view_get_window (GTK_TEXT_VIEW (view),
                                                      GTK_TEXT_WINDOW_TEXT);
        if (window)
        {
            rect.width = SAFE_GDK_WINDOW_GET_WIDTH (window);
            gdk_window_invalidate_rect (window, &rect, FALSE);
        }
    }
}


static void
add_line_mark (MooTextView *view,
               MooLineMark *mark)
{
    view->priv->line_marks = g_slist_prepend (view->priv->line_marks,
                                              g_object_ref (mark));
    g_signal_connect_swapped (mark, "changed",
                              G_CALLBACK (line_mark_changed), view);
}

static void
remove_line_mark (MooTextView *view,
                  MooLineMark *mark)
{
    if (g_slist_find (view->priv->line_marks, mark))
    {
        view->priv->line_marks = g_slist_remove (view->priv->line_marks, mark);
        g_signal_handlers_disconnect_by_func (mark, (gpointer) line_mark_changed, view);
        g_object_unref (mark);
    }
}


static void
line_mark_deleted (MooTextView *view,
                   MooLineMark *mark)
{
    if (_moo_line_mark_get_pretty (mark))
        gtk_widget_queue_draw (GTK_WIDGET (view));

    remove_line_mark (view, mark);
}


static void
line_mark_changed (MooTextView *view,
                   MooLineMark *mark)
{
    invalidate_line (view, moo_line_mark_get_line (mark), TRUE, TRUE);
}


static void
line_mark_moved (MooTextView *view,
                 MooLineMark *mark)
{
    invalidate_line (view, moo_line_mark_get_line (mark), TRUE, TRUE);

    if (_moo_line_mark_get_pretty (mark))
        gtk_widget_queue_draw (GTK_WIDGET (view));
}


static void
line_mark_added (MooTextView *view,
                 MooLineMark *mark)
{
    g_return_if_fail (!g_slist_find (view->priv->line_marks, mark));

    add_line_mark (view, mark);

    if (!moo_line_mark_get_visible (mark))
        return;

    _moo_line_mark_set_pretty (mark, TRUE);

    if (gtk_widget_get_realized (GTK_WIDGET (view)))
        _moo_line_mark_realize (mark, GTK_WIDGET (view));

    invalidate_line (view, moo_line_mark_get_line (mark), TRUE, TRUE);
}


static void
fold_added (MooTextView *view,
            MooFold     *fold)
{
    if (view->priv->enable_folding)
        invalidate_line (view, _moo_fold_get_start (fold), TRUE, fold->collapsed);
}


static void
fold_deleted (MooTextView *view)
{
    if (view->priv->enable_folding)
        gtk_widget_queue_draw (GTK_WIDGET (view));
}


static void
fold_toggled (MooTextView        *view,
              MooFold            *fold)
{
    if (view->priv->enable_folding)
    {
        if (fold)
        {
            int fold_start = _moo_fold_get_start (fold);
            if (fold_start >= 0)
                invalidate_line (view, fold_start, TRUE, TRUE);
        }
        else
            gtk_widget_queue_draw (GTK_WIDGET (view));
    }
}


/***************************************************************************/
/* Children
 */

/* http://bugzilla.gnome.org/show_bug.cgi?id=323843 */
static int
get_border_window_size (GtkTextView      *text_view,
                        GtkTextWindowType type)
{
    if (gtk_widget_get_realized (GTK_WIDGET (text_view)) && gtk_text_view_get_window (text_view, type))
        return gtk_text_view_get_border_window_size (text_view, type);
    else
        return 0;
}


void
moo_text_view_add_child_in_border (MooTextView        *view,
                                   GtkWidget          *widget,
                                   GtkTextWindowType   which_border)
{
    MooTextViewPos pos = MOO_TEXT_VIEW_POS_INVALID;
    GtkWidget **child;
    GtkRequisition child_req = {0, 0};
    int border_size = 0;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));
    g_return_if_fail (GTK_IS_WIDGET (widget));
    g_return_if_fail (gtk_widget_get_parent (widget) == NULL);

    switch (which_border)
    {
        case GTK_TEXT_WINDOW_LEFT:
            pos = MOO_TEXT_VIEW_POS_LEFT;
            break;
        case GTK_TEXT_WINDOW_RIGHT:
            pos = MOO_TEXT_VIEW_POS_RIGHT;
            break;
        case GTK_TEXT_WINDOW_TOP:
            pos = MOO_TEXT_VIEW_POS_TOP;
            break;
        case GTK_TEXT_WINDOW_BOTTOM:
            pos = MOO_TEXT_VIEW_POS_BOTTOM;
            break;
        default:
            g_return_if_reached ();
    }

    g_return_if_fail (pos < MOO_TEXT_VIEW_POS_INVALID);

    child = &view->priv->children[pos];
    g_return_if_fail (*child == NULL);

    g_object_ref_sink (widget);

    *child = widget;

    if (gtk_widget_get_visible (widget))
    {
        gtk_widget_size_request (widget, &child_req);

        switch (which_border)
        {
            case GTK_TEXT_WINDOW_LEFT:
            case GTK_TEXT_WINDOW_RIGHT:
                border_size = child_req.width;
                break;
            case GTK_TEXT_WINDOW_TOP:
            case GTK_TEXT_WINDOW_BOTTOM:
                border_size = child_req.height;
                break;
            default:
                g_assert_not_reached ();
        }

        gtk_text_view_set_border_window_size (GTK_TEXT_VIEW (view),
                                              which_border, MIN (1, border_size));
        lower_border_window (GTK_TEXT_VIEW (view), pos);
    }

    gtk_text_view_add_child_in_window (GTK_TEXT_VIEW (view), widget,
                                       GTK_TEXT_WINDOW_WIDGET, 0, 0);
}


static void __attribute__((unused))
moo_text_view_size_request (G_GNUC_UNUSED GtkWidget      *widget,
                            G_GNUC_UNUSED GtkRequisition *requisition)
{
    guint i;
    MooTextView *view;
    GtkTextView *text_view;

    view = MOO_TEXT_VIEW (widget);
    text_view = GTK_TEXT_VIEW (widget);

    for (i = 0; i < 4; i++)
    {
        int border_size = 0;
        GtkWidget *child = view->priv->children[i];
        GtkRequisition child_req;

        if (child && gtk_widget_get_visible (GTK_WIDGET (child)))
            gtk_widget_size_request (child, &child_req);
        else
            child_req.width = child_req.height = 0;

        if (child)
        {
            int old_size;

            switch (i)
            {
                case MOO_TEXT_VIEW_POS_LEFT:
                case MOO_TEXT_VIEW_POS_RIGHT:
                    border_size = child_req.width;
                    break;
                case MOO_TEXT_VIEW_POS_TOP:
                case MOO_TEXT_VIEW_POS_BOTTOM:
                    border_size = child_req.height;
                    break;
            }

            old_size = get_border_window_size (text_view,
                                               window_types[i]);
            gtk_text_view_set_border_window_size (text_view,
                                                  window_types[i],
                                                  border_size);
            if (!old_size)
                lower_border_window (GTK_TEXT_VIEW (view), i);
        }
    }

    /* GTK3: size_request removed; override get_preferred_width/height instead */
}


/* Update GtkTextView border-window sizes to match each border child's
 * preferred size. Run before chaining to the parent's get_preferred_*
 * vfuncs so the parent accounts for the borders. */
static void
moo_text_view_update_border_windows (MooTextView *view)
{
    GtkTextView *text_view = GTK_TEXT_VIEW (view);
    guint i;

    for (i = 0; i < 4; i++)
    {
        GtkWidget *child = view->priv->children[i];
        GtkRequisition child_req = {0, 0};
        int border_size = 0;
        int old_size;

        if (!child)
            continue;

        if (gtk_widget_get_visible (child))
            gtk_widget_get_preferred_size (child, NULL, &child_req);

        switch (i)
        {
            case MOO_TEXT_VIEW_POS_LEFT:
            case MOO_TEXT_VIEW_POS_RIGHT:
                border_size = child_req.width;
                break;
            case MOO_TEXT_VIEW_POS_TOP:
            case MOO_TEXT_VIEW_POS_BOTTOM:
                border_size = child_req.height;
                break;
            default:
                continue;
        }

        old_size = get_border_window_size (text_view, window_types[i]);
        if (old_size != border_size)
        {
            gtk_text_view_set_border_window_size (text_view,
                                                  window_types[i],
                                                  border_size);
            if (!old_size)
                lower_border_window (text_view, i);
        }
    }
}


static void
moo_text_view_get_preferred_width (GtkWidget *widget,
                                   gint      *minimum,
                                   gint      *natural)
{
    moo_text_view_update_border_windows (MOO_TEXT_VIEW (widget));
    GTK_WIDGET_CLASS (moo_text_view_parent_class)->get_preferred_width
        (widget, minimum, natural);
}


static void
moo_text_view_get_preferred_height (GtkWidget *widget,
                                    gint      *minimum,
                                    gint      *natural)
{
    moo_text_view_update_border_windows (MOO_TEXT_VIEW (widget));
    GTK_WIDGET_CLASS (moo_text_view_parent_class)->get_preferred_height
        (widget, minimum, natural);
}


static void
moo_text_view_size_allocate (GtkWidget     *widget,
                             GtkAllocation *allocation)
{
    guint i;
    int right, left, bottom, top, border_width;
    MooTextView *view;
    GtkTextView *text_view;

    view = MOO_TEXT_VIEW (widget);
    text_view = GTK_TEXT_VIEW (widget);

    GTK_WIDGET_CLASS(moo_text_view_parent_class)->size_allocate (widget, allocation);

    border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));

    right = get_border_window_size (text_view, GTK_TEXT_WINDOW_RIGHT);
    left = get_border_window_size (text_view, GTK_TEXT_WINDOW_LEFT);
    bottom = get_border_window_size (text_view, GTK_TEXT_WINDOW_BOTTOM);
    top = get_border_window_size (text_view, GTK_TEXT_WINDOW_TOP);

    for (i = 0; i < 4; i++)
    {
        GtkWidget *child = view->priv->children[i];
        GtkAllocation child_alloc = {0, 0, 0, 0};
        GtkRequisition child_req;

        if (!child || !gtk_widget_get_visible (GTK_WIDGET (child)))
            continue;

        child_alloc.x = left + border_width;
        child_alloc.y = top + border_width;
        gtk_widget_get_child_requisition (child, &child_req);

        switch (i)
        {
            case MOO_TEXT_VIEW_POS_RIGHT:
                child_alloc.x = MAX (allocation->width - border_width - right, 0);
                // fallthrough
            case MOO_TEXT_VIEW_POS_LEFT:
                child_alloc.width = child_req.width;
                child_alloc.height = MAX (allocation->height - 2*border_width - top - bottom, 1);
                break;

            case MOO_TEXT_VIEW_POS_BOTTOM:
                child_alloc.y = MAX (allocation->height - bottom - border_width, 0);
                // fallthrough
            case MOO_TEXT_VIEW_POS_TOP:
                child_alloc.height = child_req.height;
                child_alloc.width = MAX (allocation->width - 2*border_width - left - right, 1);
                break;
        }

        gtk_text_view_move_child (text_view, child, child_alloc.x, child_alloc.y);
        gtk_widget_size_allocate (child, &child_alloc);
    }
}


static void
moo_text_view_remove (GtkContainer *container,
                      GtkWidget    *widget)
{
    guint i;
    MooTextView *view = MOO_TEXT_VIEW (container);

    if (widget == view->priv->qs.evbox)
    {
        view->priv->qs.in_search = FALSE;
        view->priv->qs.evbox = NULL;
        view->priv->qs.entry = NULL;
        view->priv->qs.case_sensitive = NULL;
        view->priv->qs.regex = NULL;
    }

    for (i = 0; i < 4; ++i)
    {
        if (view->priv->children[i] == widget)
        {
            view->priv->children[i] = NULL;
            g_object_unref (widget);
            gtk_text_view_set_border_window_size (GTK_TEXT_VIEW (view),
                                                  window_types[i], 0);
            break;
        }
    }

    GTK_CONTAINER_CLASS(moo_text_view_parent_class)->remove (container, widget);
}


/***************************************************************************/
/* Search
 */

static void
quick_search_set_widgets_from_flags (MooTextView *view);

static void
moo_text_view_set_quick_search_flags (MooTextView        *view,
                                      MooTextSearchFlags  flags)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (flags != view->priv->qs.flags)
    {
        view->priv->qs.flags = flags;

        if (view->priv->qs.evbox)
            quick_search_set_widgets_from_flags (view);

        g_object_notify (G_OBJECT (view), "quick-search-flags");
    }
}


static void
quick_search_option_toggled (MooTextView *view)
{
    MooTextSearchFlags flags = 0;

    if (!gtk_toggle_button_get_active (view->priv->qs.case_sensitive))
        flags |= MOO_TEXT_SEARCH_CASELESS;
    if (gtk_toggle_button_get_active (view->priv->qs.regex))
        flags |= MOO_TEXT_SEARCH_REGEX;

    moo_text_view_set_quick_search_flags (view, flags);

    if (MOO_IS_EDIT_VIEW (view))
        moo_prefs_set_int (moo_edit_setting (MOO_EDIT_PREFS_QUICK_SEARCH_FLAGS), flags);
}


static void
quick_search_set_widgets_from_flags (MooTextView *view)
{
    g_signal_handlers_block_by_func (view->priv->qs.case_sensitive,
                                     quick_search_option_toggled, view);
    g_signal_handlers_block_by_func (view->priv->qs.regex,
                                     quick_search_option_toggled, view);

    gtk_toggle_button_set_active (view->priv->qs.case_sensitive,
                                  !(view->priv->qs.flags & MOO_TEXT_SEARCH_CASELESS));
    gtk_toggle_button_set_active (view->priv->qs.regex,
                                  view->priv->qs.flags & MOO_TEXT_SEARCH_REGEX);

    g_signal_handlers_unblock_by_func (view->priv->qs.case_sensitive,
                                       quick_search_option_toggled, view);
    g_signal_handlers_unblock_by_func (view->priv->qs.regex,
                                       quick_search_option_toggled, view);
}


static void
scroll_selection_onscreen (GtkTextView *text_view)
{
    GtkTextIter iter;
    GtkTextBuffer *buffer;
    GdkRectangle rect, visible_rect;

    buffer = gtk_text_view_get_buffer (text_view);
    gtk_text_buffer_get_iter_at_mark (buffer, &iter,
                                      gtk_text_buffer_get_selection_bound (buffer));
    gtk_text_view_scroll_to_iter (text_view, &iter, 0.0, FALSE, 0.0, 0.0);

    gtk_text_buffer_get_iter_at_mark (buffer, &iter,
                                      gtk_text_buffer_get_insert (buffer));

    gtk_text_view_get_iter_location (text_view, &iter, &rect);
    gtk_text_view_get_visible_rect (text_view, &visible_rect);

    if (rect.x < visible_rect.x || rect.y < visible_rect.y ||
        rect.x + rect.width > visible_rect.x + visible_rect.width ||
        rect.y + rect.height > visible_rect.y + visible_rect.height)
            gtk_text_view_scroll_to_iter (text_view, &iter, 0.0, FALSE, 0.0, 0.0);
}


static void
quick_search_message (MooTextView *view,
                      const char  *msg)
{
    moo_text_view_message (view, msg);
}


static void
quick_search_find_from (MooTextView *view,
                        const char  *text,
                        GtkTextIter *start)
{
    gboolean found;
    GtkTextIter match_start, match_end;
    GtkTextBuffer *buffer;

    if (view->priv->qs.flags & MOO_TEXT_SEARCH_REGEX)
    {
        GError *error = NULL;
        GRegex *re = g_regex_new (text, 0, 0, &error);

        if (!re)
        {
            char *msg = g_strdup_printf ("Invalid pattern '%s'", text);
            quick_search_message (view, msg);
            g_free (msg);
            return;
        }

        g_regex_unref (re);
    }

    buffer = get_buffer (view);
    found = moo_text_search_forward (start, text, view->priv->qs.flags,
                                     &match_start, &match_end, NULL);

    if (!found)
    {
        GtkTextIter iter;

        gtk_text_buffer_get_start_iter (buffer, &iter);

        if (!gtk_text_iter_equal (start, &iter))
            found = moo_text_search_forward (&iter, text, view->priv->qs.flags,
                                             &match_start, &match_end, NULL);
    }

    if (found)
    {
        gtk_text_buffer_select_range (buffer, &match_end, &match_start);
        scroll_selection_onscreen (GTK_TEXT_VIEW (view));
    }
    else
    {
        char *message;

        if (view->priv->qs.flags & MOO_TEXT_SEARCH_REGEX)
            message = g_strdup_printf ("Pattern '%s' not found", text);
        else
            message = g_strdup_printf ("Text '%s' not found", text);

        quick_search_message (view, message);
        g_free (message);
    }
}


static void
quick_search_find (MooTextView *view,
                   const char  *text)
{
    GtkTextIter iter1, iter2, insert;
    GtkTextBuffer *buffer = get_buffer (view);

    if (gtk_text_buffer_get_selection_bounds (buffer, &iter1, &iter2))
    {
        gtk_text_buffer_get_iter_at_mark (buffer, &insert, gtk_text_buffer_get_insert (buffer));

        if (gtk_text_iter_equal (&iter1, &insert))
        {
            gtk_text_buffer_place_cursor (buffer, &insert);
            quick_search_find_from (view, text, &insert);
            return;
        }
    }

    quick_search_find_from (view, text, &iter1);
}


static void
quick_search_find_next (MooTextView *view,
                        GtkEntry    *entry,
                        gboolean     from_start)
{
    const char *text;
    GtkTextIter start;
    GtkTextBuffer *buffer = get_buffer (view);

    text = gtk_entry_get_text (entry);

    if (text[0])
    {
        if (from_start)
            gtk_text_buffer_get_start_iter (buffer, &start);
        else
            gtk_text_buffer_get_iter_at_mark (buffer, &start, gtk_text_buffer_get_insert (buffer));
        quick_search_find_from (view, text, &start);
    }
}


static void
search_entry_changed (MooTextView *view,
                      GtkEntry    *entry)
{
    const char *text;

    if (!view->priv->qs.in_search)
        return;

    text = gtk_entry_get_text (entry);

    if (text[0])
        quick_search_find (view, text);
}


static gboolean
search_entry_focus_out (MooTextView *view)
{
    moo_text_view_stop_quick_search (view);
    return FALSE;
}


static gboolean
search_entry_key_press (MooTextView *view,
                        GdkEventKey *event,
                        GtkEntry    *entry)
{
    if (!view->priv->qs.in_search)
        return FALSE;

    switch (event->keyval)
    {
        case GDK_KEY_Escape:
            moo_text_view_stop_quick_search (view);
            return TRUE;

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            quick_search_find_next (view, entry,
                                    event->state & GDK_CONTROL_MASK);
            return TRUE;
    }

    return FALSE;
}


static void
moo_text_view_start_quick_search (MooTextView *view)
{
    char *text = NULL;
    GtkTextIter iter1, iter2;
    GtkTextBuffer *buffer;

    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (view->priv->qs.in_search)
        return;

    if (!view->priv->qs.entry)
    {
        QsBoxXml *xml;

        xml = qs_box_xml_new ();

        view->priv->qs.evbox = GTK_WIDGET (xml->QsBox);
        g_return_if_fail (view->priv->qs.evbox != NULL);

        view->priv->qs.entry = GTK_WIDGET (xml->entry);
        view->priv->qs.case_sensitive = GTK_TOGGLE_BUTTON (xml->case_sensitive);
        view->priv->qs.regex = GTK_TOGGLE_BUTTON (xml->regex);

        g_signal_connect_swapped (view->priv->qs.entry, "changed",
                                  G_CALLBACK (search_entry_changed), view);
        g_signal_connect_swapped (view->priv->qs.entry, "focus-out-event",
                                  G_CALLBACK (search_entry_focus_out), view);
        g_signal_connect_swapped (view->priv->qs.entry, "key-press-event",
                                  G_CALLBACK (search_entry_key_press), view);

        g_signal_connect_swapped (view->priv->qs.case_sensitive, "toggled",
                                  G_CALLBACK (quick_search_option_toggled), view);
        g_signal_connect_swapped (view->priv->qs.regex, "toggled",
                                  G_CALLBACK (quick_search_option_toggled), view);
        quick_search_set_widgets_from_flags (view);

        moo_text_view_add_child_in_border (view, view->priv->qs.evbox,
                                           GTK_TEXT_WINDOW_BOTTOM);
    }

    buffer = get_buffer (view);

    if (gtk_text_buffer_get_selection_bounds (buffer, &iter1, &iter2) &&
        gtk_text_iter_get_line (&iter1) == gtk_text_iter_get_line (&iter2))
    {
        text = gtk_text_buffer_get_slice (buffer, &iter1, &iter2, TRUE);
    }

    if (text)
        gtk_entry_set_text (GTK_ENTRY (view->priv->qs.entry), text);

    gtk_widget_show (view->priv->qs.evbox);
    gtk_widget_grab_focus (view->priv->qs.entry);

    view->priv->qs.in_search = TRUE;

    g_free (text);
}


static void
moo_text_view_stop_quick_search (MooTextView *view)
{
    g_return_if_fail (MOO_IS_TEXT_VIEW (view));

    if (view->priv->qs.in_search)
    {
        view->priv->qs.in_search = FALSE;
        gtk_widget_hide (view->priv->qs.evbox);
        gtk_widget_grab_focus (GTK_WIDGET (view));
    }
}


static gboolean
start_quick_search (MooTextView *view)
{
    if (view->priv->qs.enable)
    {
        moo_text_view_start_quick_search (view);
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}
