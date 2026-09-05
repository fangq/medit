/*
 *   moospellcheck.cpp
 *
 *   gspell-based spell-checker integration for MooEditView.
 *
 *   Architecture:
 *
 *   * GspellChecker is owned per GtkTextBuffer (one per document — medit
 *     supports multiple views sharing a single buffer, so the checker is
 *     installed on the buffer and the highlighting is enabled per view).
 *   * The view-level setup (squiggle highlight + right-click suggestion
 *     popover + language menu) is enabled by gspell_text_view_basic_setup().
 *     gspell attaches its signal handlers via weak refs, so view disposal
 *     cleans up automatically.
 *
 *   For commit #2 the checker is hard-coded to en_US and always enabled
 *   on attach.  Commit #3 wires this to MOO_EDIT_PREFS_SPELL_*.
 *
 *   Copyright (C) 2026 — part of medit.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#include "config.h"
#include "mooedit/moospellcheck.h"
#include "mooedit/mooeditprefs.h"
#include "mooedit/mootextbuffer.h"
#include "mooedit/mootextview.h"
#include "mooedit/mootextstylescheme.h"
#include "mooedit/moolang.h"
#include "mooutils/mooprefs.h"
#include "gtksourceview/gtksourcestylescheme.h"

#ifdef MOO_BUILD_SPELL
#  include <gspell/gspell.h>

/* gspell's misspelled-word tag is anonymous (created with name=NULL) and
 * uses PANGO_UNDERLINE_SINGLE + a theme-derived underline-rgba.  On dark
 * themes the default colour is dim red, low contrast.  Override to a
 * brighter red for visibility.
 *
 * gtk_text_buffer_create_tag adds the tag to the table FIRST, then sets
 * its properties — so at tag-added time underline-rgba isn't set yet.
 * Defer the override to notify::underline-rgba; by then both `underline`
 * and `underline-rgba` are populated by gspell and we can identify the
 * tag and overwrite it.
 *
 * Also tried PANGO_UNDERLINE_ERROR for a wavy line: works in a vanilla
 * GtkTextView but renders straight in medit's MooTextView.  Root cause
 * not yet identified (Pango ≥ 1.50 + same theme/font); for now we keep
 * the straight line but make it bright red instead of dim.  Drawing a
 * cairo wave by hand in MooTextView's draw chain is a future option.
 */
static void
on_tag_underline_rgba_notify (GObject       *object,
                              G_GNUC_UNUSED GParamSpec *pspec,
                              gpointer       data)
{
    GtkTextTag      *tag   = GTK_TEXT_TAG (object);
    GtkTextTagTable *table = GTK_IS_TEXT_TAG_TABLE (data) ? GTK_TEXT_TAG_TABLE (data) : NULL;
    char            *name  = NULL;

    g_object_get (tag, "name", &name, (const char*) NULL);

    /* Anonymous tag with rgba set → gspell's misspelled-word tag.
     * Disconnect so our own override doesn't recurse. */
    if (name == NULL)
    {
        /* The colour was chosen at attach time based on the view's
         * theme luminance and stashed on the tag-table.  Fall back to
         * a generic bright red if anything is missing. */
        GdkRGBA           fallback = { 1.0, 0.4, 0.4, 1.0 };
        const GdkRGBA    *chosen   = (const GdkRGBA *) (table
            ? g_object_get_data (G_OBJECT (table), "moo-spell-underline-rgba")
            : NULL);
        const GdkRGBA    *use      = chosen ? chosen : &fallback;

        g_signal_handlers_disconnect_by_func (
            tag, (gpointer) on_tag_underline_rgba_notify, table);
        g_object_set (tag,
                      "underline-rgba", use,
                      (const char*) NULL);

        /* Force max priority so any other underline-set tag in the same
         * range can't override us. */
        if (table != NULL)
        {
            int size = gtk_text_tag_table_get_size (table);
            if (size > 0)
                gtk_text_tag_set_priority (tag, size - 1);
        }
    }
    g_free (name);
}

static void
on_spell_tag_added (GtkTextTagTable *table,
                    GtkTextTag      *tag,
                    G_GNUC_UNUSED gpointer user_data)
{
    char *name = NULL;

    g_object_get (tag, "name", &name, (const char*) NULL);

    /* Anonymous tag — most likely gspell's about-to-be-styled misspelled
     * tag.  Watch for underline-rgba to flip from unset to set.  Pass
     * the tag table so the notify handler can bump priority. */
    if (name == NULL)
    {
        g_signal_connect (tag, "notify::underline-rgba",
                          G_CALLBACK (on_tag_underline_rgba_notify), table);
    }
    g_free (name);
}

/* Pick a misspelled-word underline colour appropriate for the current
 * theme.  Stash it on the tag-table so the notify handler can read it
 * back when gspell finalises the tag's properties.  The colour is
 * chosen once per buffer; if the user later switches themes mid-session,
 * existing buffers keep the old colour — a slight imperfection that
 * could be addressed by hooking the widget's "style-updated" signal in
 * a future polish pass. */
static void
update_underline_rgba_for_theme (GtkTextTagTable *table, GtkWidget *view_widget)
{
    GdkRGBA   bg = { 1.0, 1.0, 1.0, 1.0 };
    GdkRGBA  *chosen;
    double    luma;
    gboolean  bg_from_scheme = FALSE;

    /* Prefer the GtkSourceView style-scheme's "text" background — that's
     * what actually appears behind the editor text once the user picks a
     * scheme like Oblivion.  Querying GtkStyleContext alone would return
     * the underlying GTK theme bg (typically Adwaita-light) and pick the
     * wrong shade. */
    if (MOO_IS_TEXT_VIEW (view_widget))
    {
        MooTextStyleScheme *scheme =
            moo_text_view_get_style_scheme (MOO_TEXT_VIEW (view_widget));
        if (scheme != NULL)
        {
            GtkSourceStyle *text_style =
                gtk_source_style_scheme_get_style (
                    GTK_SOURCE_STYLE_SCHEME (scheme), "text");
            if (text_style != NULL)
            {
                gchar    *bg_str = NULL;
                gboolean  bg_set = FALSE;
                g_object_get (text_style,
                              "background",     &bg_str,
                              "background-set", &bg_set,
                              (const char*) NULL);
                if (bg_set && bg_str && gdk_rgba_parse (&bg, bg_str))
                    bg_from_scheme = TRUE;
                g_free (bg_str);
            }
        }
    }

    /* Fallback: GTK theme view bg. */
    if (!bg_from_scheme)
    {
        GtkStyleContext *ctx = gtk_widget_get_style_context (view_widget);
        gtk_style_context_save (ctx);
        gtk_style_context_add_class (ctx, GTK_STYLE_CLASS_VIEW);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_style_context_get_background_color (ctx,
            gtk_style_context_get_state (ctx), &bg);
        G_GNUC_END_IGNORE_DEPRECATIONS
        gtk_style_context_restore (ctx);
    }

    luma = 0.299 * bg.red + 0.587 * bg.green + 0.114 * bg.blue;

    chosen = g_new (GdkRGBA, 1);
    if (luma <= 0.5)
    {
        /* Dark background (Oblivion etc.) — washed-out red blends into
         * gray.  Use a bright pink-red that still reads as "error". */
        chosen->red   = 1.0;
        chosen->green = 0.55;
        chosen->blue  = 0.55;
        chosen->alpha = 1.0;
    }
    else
    {
        /* Light background — deeper saturated red. */
        chosen->red   = 0.85;
        chosen->green = 0.10;
        chosen->blue  = 0.10;
        chosen->alpha = 1.0;
    }

    g_object_set_data_full (G_OBJECT (table),
                            "moo-spell-underline-rgba",
                            chosen,
                            g_free);
}

/* GtkTextTagTable callback: update any already-created gspell tag with
 * the new underline colour.  Recognised by the fingerprint we already
 * use (anonymous + underline-rgba-set). */
static void
restyle_existing_gspell_tag (GtkTextTag *tag, gpointer data)
{
    const GdkRGBA *new_color = (const GdkRGBA *) data;
    char          *name      = NULL;
    gboolean       rgba_set  = FALSE;

    g_object_get (tag,
                  "name",               &name,
                  "underline-rgba-set", &rgba_set,
                  (const char*) NULL);
    if (name == NULL && rgba_set)
        g_object_set (tag, "underline-rgba", new_color, (const char*) NULL);
    g_free (name);
}

static void
install_spell_tag_hook (GtkTextBuffer *buffer, GtkWidget *view_widget)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table (buffer);

    /* Re-pick colour every time attach() runs — covers the case where
     * the user opens the same buffer in a second view after a theme
     * change. */
    update_underline_rgba_for_theme (table, view_widget);

    if (g_object_get_data (G_OBJECT (table), "moo-spell-tag-hook"))
        return;
    g_signal_connect (table, "tag-added",
                      G_CALLBACK (on_spell_tag_added), NULL);
    g_object_set_data (G_OBJECT (table), "moo-spell-tag-hook",
                       GINT_TO_POINTER (1));
}
#endif /* MOO_BUILD_SPELL */

#ifdef MOO_BUILD_SPELL
/* Point Enchant (which backs gspell) at a medit-specific config dir so
 * "Add to Dictionary" entries don't pollute or share the system-wide
 * ~/.config/enchant/.  Called once per process, before the first
 * GspellChecker is created.  If ENCHANT_CONFIG_DIR is already set in
 * the user's environment, respect that — power users may want to share
 * a personal dict across GTK apps. */
static void
init_enchant_config_dir_once (void)
{
    static gsize once = 0;
    if (g_once_init_enter (&once))
    {
        if (g_getenv ("ENCHANT_CONFIG_DIR") == NULL)
        {
            char        *dir = g_build_filename (g_get_user_config_dir (),
                                                 "medit", "enchant", (const char*) NULL);
            mgw_errno_t  err = MGW_E_NOERROR;
            mgw_mkdir_with_parents (dir, 0700, &err);
            g_setenv ("ENCHANT_CONFIG_DIR", dir, TRUE);
            g_free (dir);
        }
        g_once_init_leave (&once, 1);
    }
}
#endif /* MOO_BUILD_SPELL */

void
_moo_spell_check_attach (G_GNUC_UNUSED MooEditView *view)
{
#ifdef MOO_BUILD_SPELL
    GtkTextBuffer       *buffer;
    GspellTextBuffer    *gbuffer;
    GspellTextView      *gview;

    g_return_if_fail (MOO_IS_EDIT_VIEW (view));

    /* Redirect Enchant's user-dict dir before any gspell init runs. */
    init_enchant_config_dir_once ();

    buffer  = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    gbuffer = gspell_text_buffer_get_from_gtk_text_buffer (buffer);

    /* Hook the buffer's tag table so when gspell adds its anonymous
     * misspelled-word tag we can override the underline-rgba (default
     * picks the theme's dim error colour which is hard to read on dark
     * themes).  Idempotent across attach() calls; pass the view so the
     * helper can probe its style context for the bg luminance. */
    install_spell_tag_hook (buffer, GTK_WIDGET (view));

    /* Install the en_US checker on the buffer if no other view of the
     * same buffer has already done so.  Buffer owns the ref. */
    if (gspell_text_buffer_get_spell_checker (gbuffer) == NULL)
    {
        const GspellLanguage *lang;
        GspellChecker        *checker;

        lang = gspell_language_lookup ("en_US");
        /* lookup may return NULL if no en_US dict is installed; gspell
         * accepts NULL and defaults to the user's locale (we fall back
         * gracefully rather than refusing to attach). */
        checker = gspell_checker_new (lang);
        gspell_text_buffer_set_spell_checker (gbuffer, checker);
        g_object_unref (checker);
    }

    gview = gspell_text_view_get_from_gtk_text_view (GTK_TEXT_VIEW (view));
    /* basic_setup() turns on inline checking + the language menu; we
     * immediately override the inline-checking flag with the value
     * dictated by MOO_EDIT_PREFS_SPELL_ENABLED + scope (next call). */
    gspell_text_view_basic_setup (gview);

    /* Honor prefs from the start (else newly-created views always start
     * with spell-check on, regardless of the user setting). */
    _moo_spell_check_apply_prefs (view);
#endif
}

void
_moo_spell_check_detach (G_GNUC_UNUSED MooEditView *view)
{
#ifdef MOO_BUILD_SPELL
    GspellTextView *gview;

    if (!MOO_IS_EDIT_VIEW (view))
        return;

    /* Disable just the inline highlight on this view; leave the
     * buffer-level checker in place so re-attach (e.g. after toggling
     * the pref) is cheap.  When the view is being disposed gspell's
     * own weak ref drops its per-view state automatically. */
    gview = gspell_text_view_get_from_gtk_text_view (GTK_TEXT_VIEW (view));
    if (gview != NULL)
        gspell_text_view_set_inline_spell_checking (gview, FALSE);
#endif
}

#ifdef MOO_BUILD_SPELL
/* Decide whether the view's buffer is "source code" — anything with a
 * MooLang attached counts.  Plain text and unrecognised file types fall
 * back to prose mode.  Used to interpret scope="auto". */
static gboolean
spell_buffer_is_code (GtkTextBuffer *buffer)
{
    MooTextBuffer *mbuf;
    MooLang       *lang;

    if (!MOO_IS_TEXT_BUFFER (buffer))
        return FALSE;

    mbuf = MOO_TEXT_BUFFER (buffer);
    lang = moo_text_buffer_get_lang (mbuf);
    return lang != NULL;
}
#endif

void
_moo_spell_check_apply_prefs (G_GNUC_UNUSED MooEditView *view)
{
#ifdef MOO_BUILD_SPELL
    GtkTextBuffer  *buffer;
    GspellTextView *gview;
    gboolean        enabled;
    const char     *scope_str;
    gboolean        effective_on;

    g_return_if_fail (MOO_IS_EDIT_VIEW (view));

    /* Skip if the view is already being torn down — apply_prefs() can
     * fire late during prefs-save-on-close and dereferencing the view
     * after gtk_widget_destroy() segfaults. */
    if (gtk_widget_in_destruction (GTK_WIDGET (view)))
        return;

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    if (!GTK_IS_TEXT_BUFFER (buffer))
        return;

    gview     = gspell_text_view_get_from_gtk_text_view (GTK_TEXT_VIEW (view));
    enabled   = moo_prefs_get_bool   (moo_edit_setting (MOO_EDIT_PREFS_SPELL_ENABLED));
    scope_str = moo_prefs_get_string (moo_edit_setting (MOO_EDIT_PREFS_SPELL_SCOPE));

    if (!enabled)
    {
        effective_on = FALSE;
    }
    else if (scope_str && g_str_equal (scope_str, "all"))
    {
        /* User wants spell-check everywhere, regardless of language. */
        effective_on = TRUE;
    }
    else if (scope_str && g_str_equal (scope_str, "code"))
    {
        /* "Comments + strings only" — until commit #4 implements the
         * proper filter via gspell-no-spell-check tags, treat code mode
         * as "off for source files, on for prose".  Once the filter
         * lands we'll set effective_on=TRUE here and let the filter
         * suppress non-comment/string regions. */
        effective_on = !spell_buffer_is_code (buffer);
    }
    else
    {
        /* "auto" (default): on for prose, off for code (transitional
         * — same as "code" path above until commit #4). */
        effective_on = !spell_buffer_is_code (buffer);
    }

    if (gview != NULL)
        gspell_text_view_set_inline_spell_checking (gview, effective_on);

    /* Re-pick the underline colour now that the style scheme is in
     * effect (it may not have been at attach time), and propagate the
     * new colour to any already-existing gspell tag in the table. */
    {
        GtkTextTagTable *table = gtk_text_buffer_get_tag_table (buffer);
        const GdkRGBA   *color;

        update_underline_rgba_for_theme (table, GTK_WIDGET (view));
        color = (const GdkRGBA *) g_object_get_data (
            G_OBJECT (table), "moo-spell-underline-rgba");
        if (color != NULL)
            gtk_text_tag_table_foreach (table,
                                        restyle_existing_gspell_tag,
                                        (gpointer) color);
    }
#endif
}

/* ── Popup-menu integration ────────────────────────────────────────────────
 *
 * medit overrides right-click to show a custom popup built from its
 * UI XML ("Editor/Popup"), bypassing GtkTextView's default popup-creation
 * — so the "populate-popup" signal is never emitted and gspell's normal
 * hook can't add suggestion items.  Instead we build the spell-check
 * submenu directly using the public gspell checker API and append it.
 */

#ifdef MOO_BUILD_SPELL

typedef struct {
    /* GspellChecker — owned by buffer, weak-ref'd here for safety */
    GspellChecker *checker;
    /* word and its range — copied so handlers survive the popup tear-down */
    GtkTextBuffer *buffer;
    GtkTextMark   *word_start;
    GtkTextMark   *word_end;
    char          *word;
} SpellPopupCtx;

static void
spell_popup_ctx_free (gpointer data, G_GNUC_UNUSED GClosure *closure)
{
    SpellPopupCtx *ctx = (SpellPopupCtx *) data;
    if (ctx->word_start)
        gtk_text_buffer_delete_mark (ctx->buffer, ctx->word_start);
    if (ctx->word_end)
        gtk_text_buffer_delete_mark (ctx->buffer, ctx->word_end);
    g_free (ctx->word);
    g_slice_free (SpellPopupCtx, ctx);
}

static void
on_replace_with_suggestion (GtkMenuItem *item, gpointer data)
{
    SpellPopupCtx *ctx = (SpellPopupCtx *) data;
    const char    *replacement;
    GtkTextIter    start, end;

    replacement = (const char *) g_object_get_data (G_OBJECT (item),
                                                    "moo-spell-replacement");
    if (!replacement || !ctx->word_start || !ctx->word_end)
        return;

    gtk_text_buffer_get_iter_at_mark (ctx->buffer, &start, ctx->word_start);
    gtk_text_buffer_get_iter_at_mark (ctx->buffer, &end,   ctx->word_end);

    gtk_text_buffer_begin_user_action (ctx->buffer);
    gtk_text_buffer_delete (ctx->buffer, &start, &end);
    gtk_text_buffer_insert (ctx->buffer, &start, replacement, -1);
    gtk_text_buffer_end_user_action (ctx->buffer);

    /* Tell gspell we corrected it so its session stats stay sane. */
    if (ctx->checker && ctx->word)
        gspell_checker_set_correction (ctx->checker,
                                       ctx->word, -1,
                                       replacement, -1);
}

static void
on_add_to_dictionary (G_GNUC_UNUSED GtkMenuItem *item, gpointer data)
{
    SpellPopupCtx *ctx = (SpellPopupCtx *) data;
    if (ctx->checker && ctx->word)
        gspell_checker_add_word_to_personal (ctx->checker, ctx->word, -1);
}

static void
on_ignore_word (G_GNUC_UNUSED GtkMenuItem *item, gpointer data)
{
    SpellPopupCtx *ctx = (SpellPopupCtx *) data;
    if (ctx->checker && ctx->word)
        gspell_checker_add_word_to_session (ctx->checker, ctx->word, -1);
}

/* Standard word-boundary scan: alphanumeric + apostrophes + connecting
 * punctuation grow the word; everything else terminates it.  Returns
 * TRUE if a non-empty word was found around `iter`. */
static gboolean
extract_word_at_iter (GtkTextBuffer *buffer,
                      const GtkTextIter *iter,
                      GtkTextIter *out_start,
                      GtkTextIter *out_end,
                      char **out_word)
{
    GtkTextIter start = *iter;
    GtkTextIter end   = *iter;
    GtkTextIter probe;

    /* Walk backwards while preceding char is part of a word */
    probe = start;
    while (gtk_text_iter_backward_char (&probe))
    {
        gunichar c = gtk_text_iter_get_char (&probe);
        if (!g_unichar_isalpha (c) && c != '\'' && c != 0x2019 /* ’ */)
            break;
        start = probe;
    }

    /* Walk forwards while current char is part of a word */
    while (!gtk_text_iter_is_end (&end))
    {
        gunichar c = gtk_text_iter_get_char (&end);
        if (!g_unichar_isalpha (c) && c != '\'' && c != 0x2019)
            break;
        gtk_text_iter_forward_char (&end);
    }

    if (gtk_text_iter_equal (&start, &end))
        return FALSE;

    *out_start = start;
    *out_end   = end;
    *out_word  = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
    return TRUE;
}

#endif /* MOO_BUILD_SPELL */

void
_moo_spell_check_populate_popup (G_GNUC_UNUSED MooEditView *view,
                                 G_GNUC_UNUSED GtkMenu     *menu,
                                 G_GNUC_UNUSED int          widget_x,
                                 G_GNUC_UNUSED int          widget_y)
{
#ifdef MOO_BUILD_SPELL
    GtkTextView      *tv;
    GtkTextBuffer    *buffer;
    GspellTextBuffer *gbuffer;
    GspellChecker    *checker;
    GtkTextIter       click_iter, word_start, word_end;
    char             *word = NULL;
    GSList           *suggestions, *l;
    int               bx, by;
    GtkWidget        *spell_sep, *sub, *sub_item;
    SpellPopupCtx    *ctx;
    int               i;

    g_return_if_fail (MOO_IS_EDIT_VIEW (view));
    g_return_if_fail (GTK_IS_MENU (menu));

    tv      = GTK_TEXT_VIEW (view);
    buffer  = gtk_text_view_get_buffer (tv);
    gbuffer = gspell_text_buffer_get_from_gtk_text_buffer (buffer);
    checker = gspell_text_buffer_get_spell_checker (gbuffer);
    if (checker == NULL)
        return;

    /* Resolve click position → buffer iter.  Negative coords means
     * "use the cursor" (keyboard-triggered popup). */
    if (widget_x < 0 || widget_y < 0)
    {
        gtk_text_buffer_get_iter_at_mark (buffer, &click_iter,
                                          gtk_text_buffer_get_insert (buffer));
    }
    else
    {
        gtk_text_view_window_to_buffer_coords (tv, GTK_TEXT_WINDOW_WIDGET,
                                               widget_x, widget_y, &bx, &by);
        gtk_text_view_get_iter_at_location (tv, &click_iter, bx, by);
    }

    if (!extract_word_at_iter (buffer, &click_iter, &word_start, &word_end, &word))
        return;

    /* Word found.  Spell-check it; if it's already correct, nothing to add. */
    if (gspell_checker_check_word (checker, word, -1, NULL))
    {
        g_free (word);
        return;
    }

    /* Build the shared context (one copy of the marks / word string is
     * shared across all menu-item handlers via closure-notify cleanup). */
    ctx = g_slice_new0 (SpellPopupCtx);
    ctx->checker    = checker;
    ctx->buffer     = buffer;
    ctx->word_start = gtk_text_buffer_create_mark (buffer, NULL, &word_start, TRUE);
    ctx->word_end   = gtk_text_buffer_create_mark (buffer, NULL, &word_end,   FALSE);
    ctx->word       = word;   /* takes ownership */

    /* Prepend (so the spell entries appear at the TOP of medit's
     * already-long custom popup, where the user can actually see them).
     * Order ends up: [Spelling ▸] [separator] [...medit's items...]. */
    spell_sep = gtk_separator_menu_item_new ();
    gtk_widget_show (spell_sep);
    gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), spell_sep);

    sub_item = gtk_menu_item_new_with_label ("Spelling");
    gtk_widget_show (sub_item);
    gtk_menu_shell_prepend (GTK_MENU_SHELL (menu), sub_item);
    sub = gtk_menu_new ();
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (sub_item), sub);

    suggestions = gspell_checker_get_suggestions (checker, word, -1);
    if (suggestions == NULL)
    {
        GtkWidget *none = gtk_menu_item_new_with_label ("(no suggestions)");
        gtk_widget_set_sensitive (none, FALSE);
        gtk_widget_show (none);
        gtk_menu_shell_append (GTK_MENU_SHELL (sub), none);
    }
    else
    {
        /* Cap to 10 entries so the popup stays usable. */
        for (l = suggestions, i = 0; l != NULL && i < 10; l = l->next, i++)
        {
            char      *suggestion = (char *) l->data;
            GtkWidget *mi         = gtk_menu_item_new_with_label (suggestion);

            g_object_set_data_full (G_OBJECT (mi), "moo-spell-replacement",
                                    g_strdup (suggestion), g_free);
            g_signal_connect_data (mi, "activate",
                                   G_CALLBACK (on_replace_with_suggestion),
                                   ctx, NULL, (GConnectFlags) 0);
            gtk_widget_show (mi);
            gtk_menu_shell_append (GTK_MENU_SHELL (sub), mi);
        }
    }
    g_slist_free_full (suggestions, g_free);

    /* Separator + actions. */
    {
        GtkWidget *sep  = gtk_separator_menu_item_new ();
        GtkWidget *add  = gtk_menu_item_new_with_label ("Add to Dictionary");
        GtkWidget *ign  = gtk_menu_item_new_with_label ("Ignore All");

        gtk_widget_show (sep);
        gtk_widget_show (add);
        gtk_widget_show (ign);
        gtk_menu_shell_append (GTK_MENU_SHELL (sub), sep);
        gtk_menu_shell_append (GTK_MENU_SHELL (sub), add);
        gtk_menu_shell_append (GTK_MENU_SHELL (sub), ign);

        g_signal_connect_data (add, "activate",
                               G_CALLBACK (on_add_to_dictionary),
                               ctx, NULL, (GConnectFlags) 0);
        /* Free the ctx exactly once via the last menu-item's closure. */
        g_signal_connect_data (ign, "activate",
                               G_CALLBACK (on_ignore_word),
                               ctx, spell_popup_ctx_free, (GConnectFlags) 0);
    }
#endif /* MOO_BUILD_SPELL */
}

/* ── Status-bar + menubar entry points ─────────────────────────────────── */

char *
_moo_spell_check_status_text (G_GNUC_UNUSED MooEditView *view)
{
#ifdef MOO_BUILD_SPELL
    GtkTextBuffer        *buffer;
    GspellTextBuffer     *gbuffer;
    GspellChecker        *checker;
    GspellTextView       *gview;
    const GspellLanguage *lang;
    const char           *code;
    gboolean              enabled;

    g_return_val_if_fail (MOO_IS_EDIT_VIEW (view), NULL);

    enabled = moo_prefs_get_bool (moo_edit_setting (MOO_EDIT_PREFS_SPELL_ENABLED));
    if (!enabled)
        return g_strdup ("Spell: off");

    gview = gspell_text_view_get_from_gtk_text_view (GTK_TEXT_VIEW (view));
    if (gview != NULL &&
        !gspell_text_view_get_inline_spell_checking (gview))
        return g_strdup ("Spell: off");

    buffer  = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    gbuffer = gspell_text_buffer_get_from_gtk_text_buffer (buffer);
    checker = gspell_text_buffer_get_spell_checker (gbuffer);
    if (checker == NULL)
        return g_strdup ("Spell: off");

    lang = gspell_checker_get_language (checker);
    code = lang ? gspell_language_get_code (lang) : NULL;
    return g_strdup_printf ("Spell: %s", code ? code : "default");
#else
    (void) view;
    return NULL;
#endif
}

#ifdef MOO_BUILD_SPELL
/* Common: find the word at the current cursor, return NULL if no word
 * or no spell-checker attached.  Caller g_free's. */
static char *
get_word_at_cursor (MooEditView    *view,
                    GspellChecker **out_checker,
                    GtkTextBuffer **out_buffer)
{
    GtkTextBuffer    *buffer;
    GspellTextBuffer *gbuffer;
    GspellChecker    *checker;
    GtkTextIter       cur, word_start, word_end;
    char             *word = NULL;

    buffer  = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    gbuffer = gspell_text_buffer_get_from_gtk_text_buffer (buffer);
    checker = gspell_text_buffer_get_spell_checker (gbuffer);
    if (checker == NULL)
        return NULL;

    gtk_text_buffer_get_iter_at_mark (buffer, &cur,
                                      gtk_text_buffer_get_insert (buffer));
    if (!extract_word_at_iter (buffer, &cur, &word_start, &word_end, &word))
        return NULL;

    if (out_checker) *out_checker = checker;
    if (out_buffer)  *out_buffer  = buffer;
    return word;   /* caller frees */
}
#endif /* MOO_BUILD_SPELL */

void
_moo_spell_check_add_word_at_cursor (G_GNUC_UNUSED MooEditView *view)
{
#ifdef MOO_BUILD_SPELL
    GspellChecker *checker = NULL;
    char          *word;

    g_return_if_fail (MOO_IS_EDIT_VIEW (view));
    word = get_word_at_cursor (view, &checker, NULL);
    if (word && checker)
        gspell_checker_add_word_to_personal (checker, word, -1);
    g_free (word);
#endif
}

void
_moo_spell_check_ignore_word_at_cursor (G_GNUC_UNUSED MooEditView *view)
{
#ifdef MOO_BUILD_SPELL
    GspellChecker *checker = NULL;
    char          *word;

    g_return_if_fail (MOO_IS_EDIT_VIEW (view));
    word = get_word_at_cursor (view, &checker, NULL);
    if (word && checker)
        gspell_checker_add_word_to_session (checker, word, -1);
    g_free (word);
#endif
}
