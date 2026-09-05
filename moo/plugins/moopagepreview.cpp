/*
 *   moopagepreview.cpp
 *
 *   Unified in-editor "Page Preview" side pane.  Picks a renderer per
 *   document based on filename and (optionally) a first-line marker:
 *
 *     *.md / *.markdown / *.mdx              → Markdown (md4c)
 *     *.wiki / *.wp / *.usemod               → Wiki (in-tree converter)
 *     first line "<!-- markdown -->"         → Markdown (Habitat marker)
 *     first line "<!-- wiki -->"             → Wiki
 *     anything else                          → no preview (placeholder)
 *
 *   Markdown rendering needs md4c+md4c-html at build time; if
 *   MOO_BUILD_MARKDOWN isn't defined the markdown branch shows
 *   "Markdown preview not available in this build" while wiki keeps
 *   working.  Wiki rendering is entirely in-tree (no external lib),
 *   compiled in unless MOO_BUILD_WIKI was explicitly disabled.  If
 *   neither is set the whole file compiles to a no-op
 *   _moo_page_preview_plugin_init() so plugin-builtin.cpp can call it
 *   unconditionally.
 *
 *   ─────────── Wiki syntax: attribution ───────────────────────────────
 *
 *   The wiki dialect implemented here is a clean-room C reimplementation
 *   of a pragmatic subset of Habitat's Render.pm:
 *
 *       https://github.com/fangq/habitat
 *       Copyright (C) 2009, 2010, 2014  Qianqian Fang
 *           <fangq (at) nmr.mgh.harvard.edu>   (GPL v2)
 *
 *   Habitat descends from UseModWiki 1.0 by Clifford A. Adams and
 *   Sunir Shah, which goes back through AtisWiki 0.3 (Markus Denker,
 *   1998) to Ward Cunningham's WikiWikiWeb.  Upstream copyrights are
 *   preserved by reference.
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
#include "mooedit/mooplugin.h"
#include "mooedit/mooplugin-macro.h"
#include "mooedit/mooeditwindow.h"
#include "mooedit/mooedit.h"
#include "mooutils/moostock.h"
#include "mooutils/mooi18n.h"
#include "mooutils/mooprefs.h"

#define PAGE_PREVIEW_PLUGIN_ID "PagePreview"

/* Persistent pref controlling whether the preview pane should be shown
 * the moment a window opens.  TRUE by default (the pane is small and
 * the renderer no-ops cheaply for non-Markdown buffers); set to FALSE
 * for users who'd rather have it stay hidden until they trigger it via
 * View → Panes → Page Preview.  The pane is still registered in
 * the PanesMenu regardless, so per-window show/hide is always one menu
 * click away. */
#define PAGE_PREVIEW_SHOW_PREF "Plugins/PagePreview/show"

#if defined(MOO_BUILD_MARKDOWN) || defined(MOO_BUILD_WIKI)

#include <gtk/gtk.h>
#include <string.h>
#ifdef MOO_BUILD_MARKDOWN
#include <md4c-html.h>
#endif
#include "mooapp/moohtml.h"
#include "mooedit/mooeditview.h"

typedef struct {
    MooPlugin parent;
} PagePreviewPlugin;

typedef struct {
    MooWinPlugin parent;
    MooPane     *pane;          /* registered side-pane */
    GtkWidget   *html_view;     /* MooHtml widget */

    /* Live-preview state — tracked so we can disconnect on doc switch
     * and on plugin teardown without dangling-handler crashes. */
    gulong       notify_active_doc_id;  /* on the MooEditWindow */
    GtkTextBuffer *current_buffer;      /* whichever buffer "changed" is connected to; owned */
    gulong       buffer_changed_id;     /* handler id on current_buffer */
    guint        render_timeout_id;     /* g_timeout source for debounce */
} PagePreviewWindowPlugin;

/* How long the user has to be idle (no edits) before we re-render the
 * preview.  200 ms feels live without slamming md4c on every keystroke.
 * Cheap to tune later if users want lower-latency feedback. */
#define PAGE_PREVIEW_DEBOUNCE_MS 200

#ifdef MOO_BUILD_MARKDOWN
/* md4c invokes this for every chunk of generated HTML.  Append into
 * the GString the caller passed via userdata.  Inlined for clarity —
 * the callback is hot but the work is tiny. */
static void
md4c_output_cb (const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    g_string_append_len ((GString *) userdata, text, size);
}
#endif /* MOO_BUILD_MARKDOWN */

#ifdef MOO_BUILD_WIKI
/* ════════════════════════════════════════════════════════════════════════
 * Wiki → HTML converter
 *
 * Two-pass design:
 *   1. Protected-region pass: pull <nowiki>...</nowiki>, <pre>...</pre>
 *      and <code>...</code> blocks out of the source and replace them
 *      with FS-style placeholders (\x01<N>\x01).  Their original
 *      content is stashed in `saved` and re-inserted after the rest of
 *      the pipeline has run.
 *   2. Line-oriented block pass: iterate lines, manage a block stack
 *      (ul/ol/dl/pre/table/p), apply CommonMarkup inline rules to text
 *      content.
 *
 * Conservative on purpose: anything that doesn't match a known rule is
 * left as-is (HTML-escaped earlier), so unknown wiki syntax degrades to
 * plain text rather than mangled output.
 * ════════════════════════════════════════════════════════════════════════
 */

/* TOC entry: one per "== # Heading ==" we see, plus level for indent. */
typedef struct {
    int   level;     /* 1..6, matches <hN> */
    char *title;     /* g_strdup'd display text (no leading "# ") */
    char *anchor;    /* g_strdup'd slug for href="#..." */
    char *number;    /* hierarchical section number "1.2.3" — built at
                      * heading-emit time so both the rendered <hN> and
                      * the TOC entry use the same string */
} WikiTocEntry;

typedef struct {
    GString    *out;          /* accumulating HTML */
    GPtrArray  *saved;        /* g_free-ownership of protected-region strings */
    GSList     *blocks;       /* stack of strdup'd "ul"/"ol"/"dl"/"pre"/"table" */
    GArray     *block_depth;  /* matching depth per block stack entry */
    gboolean    in_para;
    int         link_counter; /* for numbered [url] refs */

    /* TOC support — populated as headings are emitted, replayed at the
     * end when wiki_to_html post-processes the buffer for <toc> markers. */
    GPtrArray  *toc;          /* of WikiTocEntry*, owned */
    int         anchor_seq;   /* dedupe counter for duplicate heading slugs */

    /* Set of anchor names already emitted so a stray duplicate
     * "[#name]" doesn't trip libxml2's "ID already defined" warning.
     * Both the heading anchor (== # …) and the inline [#name] register
     * here. */
    GHashTable *anchors_seen;

    /* Per-level counters for hierarchical "1.2.3"-style section
     * numbers on "== # Heading ==".  Index 0 is unused (heading levels
     * are 1-based); on each numbered heading at level N we increment
     * heading_counters[N] and reset heading_counters[N+1..6]. */
    int heading_counters[7];

    /* List-nesting state.  Held separately from the generic block
     * stack because HTML5 requires nested <ul>/<ol> to live inside
     * the parent's <li>, not as a direct child of the outer list.
     * list_stack[i] is the kind ('u' / 'o' / 'd') of the i-th open
     * list, list_li_open[i] is TRUE iff that level has an unclosed
     * <li>/<dd> we're waiting to close. */
    GArray *list_stack;     /* char per element */
    GArray *list_li_open;   /* gboolean per element */
} WikiCtx;

/* ── List state-machine helpers ──────────────────────────────────────
 * The state-machine emits proper HTML5 nesting: each deeper level
 * lives inside the closest open <li>, and <li>s are closed lazily
 * (when the next sibling arrives or the list closes) so a nested
 * inner list can be inserted without first closing the outer item.
 */

static const char *
list_tag_for_kind (char k)
{
    return k == 'u' ? "ul" : k == 'o' ? "ol" : "dl";
}

/* True iff we currently have any open list block. */
static gboolean
wiki_in_list (WikiCtx *ctx)
{
    return ctx->list_stack->len > 0;
}

/* Close the current list down to the target depth (or below, if
 * `target` is less than the current depth).  After this call the
 * list stack has exactly `target` entries. */
static void
wiki_list_close_to (WikiCtx *ctx, int target)
{
    while ((int) ctx->list_stack->len > target)
    {
        int      idx       = ctx->list_stack->len - 1;
        char     kind      = g_array_index (ctx->list_stack, char,     idx);
        gboolean li_open   = g_array_index (ctx->list_li_open, gboolean, idx);
        const char *tag    = list_tag_for_kind (kind);
        const char *li_tag = (kind == 'd') ? "dd" : "li";

        if (li_open)
            g_string_append_printf (ctx->out, "</%s>\n", li_tag);
        g_string_append_printf (ctx->out, "</%s>\n", tag);

        g_array_remove_index (ctx->list_stack, idx);
        g_array_remove_index (ctx->list_li_open, idx);

        /* The inner list lived inside the parent's <li> — that <li>
         * stays open across the inner block, so close it now too. */
        if ((int) ctx->list_stack->len > 0
            && (int) ctx->list_stack->len >= target)
        {
            int      pidx = ctx->list_stack->len - 1;
            char     pk   = g_array_index (ctx->list_stack, char, pidx);
            const char *p_li = (pk == 'd') ? "dd" : "li";
            if (g_array_index (ctx->list_li_open, gboolean, pidx))
            {
                g_string_append_printf (ctx->out, "</%s>\n", p_li);
                g_array_index (ctx->list_li_open, gboolean, pidx) = FALSE;
            }
        }
    }
}

/* Open list levels until depth matches the new target.  When pushing
 * a level inside an already-open <li> we don't close that <li> —
 * leaving the inner list as a nested child of the item. */
static void
wiki_list_push_to (WikiCtx *ctx, char kind, int target)
{
    while ((int) ctx->list_stack->len < target)
    {
        int      idx     = ctx->list_stack->len;
        const char *tag  = list_tag_for_kind (kind);
        if (idx > 0)
        {
            gboolean prev_li = g_array_index (ctx->list_li_open,
                                              gboolean, idx - 1);
            if (!prev_li)
            {
                /* Need a placeholder <li> so the nested list isn't a
                 * direct child of <ul>/<ol> (which HTML5 rejects). */
                char pk = g_array_index (ctx->list_stack, char, idx - 1);
                const char *p_li = (pk == 'd') ? "dd" : "li";
                g_string_append_printf (ctx->out, "<%s>", p_li);
                g_array_index (ctx->list_li_open, gboolean, idx - 1) = TRUE;
            }
        }
        g_string_append_printf (ctx->out, "<%s>\n", tag);
        char k = kind;
        gboolean lo = FALSE;
        g_array_append_val (ctx->list_stack, k);
        g_array_append_val (ctx->list_li_open, lo);
    }
}

/* Emit a single list item at the given (kind, depth).  Handles
 * close-to-depth, open-to-depth, kind-change, and the "close
 * previous sibling <li>" step. */
static void
wiki_list_item (WikiCtx *ctx, char kind, int depth,
                const char *open_tag, const char *content)
{
    /* If at this depth the current kind doesn't match, drop down so
     * we re-open with the new kind. */
    if ((int) ctx->list_stack->len >= depth
        && g_array_index (ctx->list_stack, char, depth - 1) != kind)
    {
        wiki_list_close_to (ctx, depth - 1);
    }

    if ((int) ctx->list_stack->len > depth)
        wiki_list_close_to (ctx, depth);
    if ((int) ctx->list_stack->len < depth)
        wiki_list_push_to (ctx, kind, depth);

    int idx = depth - 1;
    const char *li_tag = (kind == 'd') ? "dd" : "li";
    if (g_array_index (ctx->list_li_open, gboolean, idx))
        g_string_append_printf (ctx->out, "</%s>\n", li_tag);
    g_string_append_printf (ctx->out, "%s%s",
                             open_tag ? open_tag : "<li>",
                             content);
    g_array_index (ctx->list_li_open, gboolean, idx) = TRUE;
}

static void
wiki_toc_entry_free (gpointer p)
{
    WikiTocEntry *e = (WikiTocEntry *) p;
    if (!e) return;
    g_free (e->title);
    g_free (e->anchor);
    g_free (e->number);
    g_free (e);
}

/* GitHub-ish slug: lowercase, runs of non-alphanum collapse to a single
 * '-', leading/trailing '-' stripped.  Returns NULL if nothing slug-able
 * is left (caller can fall back to "section-N" in that case). */
static char *
wiki_slugify (const char *text)
{
    if (!text) return NULL;
    GString *s = g_string_new (NULL);
    gboolean prev_dash = TRUE;
    for (const char *p = text; *p; p = g_utf8_next_char (p))
    {
        gunichar ch = g_utf8_get_char (p);
        if (g_unichar_isalnum (ch))
        {
            g_string_append_unichar (s, g_unichar_tolower (ch));
            prev_dash = FALSE;
        }
        else if (!prev_dash)
        {
            g_string_append_c (s, '-');
            prev_dash = TRUE;
        }
    }
    while (s->len > 0 && s->str[s->len - 1] == '-')
        g_string_truncate (s, s->len - 1);
    if (s->len == 0) { g_string_free (s, TRUE); return NULL; }
    return g_string_free (s, FALSE);
}

/* Stash a chunk of raw HTML behind a numeric placeholder so the rest
 * of the pipeline can't touch it.  Returns a fresh g_strdup'd string
 * the caller is responsible for. */
static char *
wiki_save_raw (WikiCtx *ctx, const char *html)
{
    guint idx = ctx->saved->len;
    g_ptr_array_add (ctx->saved, g_strdup (html));
    return g_strdup_printf ("\x01%u\x01", idx);
}

/* HTML-escape <, >, & in user text.  Returned string is g_strdup-owned. */
static char *
wiki_quote_html (const char *src)
{
    GString *s = g_string_sized_new (strlen (src) + 16);
    for (const char *p = src; *p; p++)
    {
        switch (*p)
        {
            case '&': g_string_append (s, "&amp;"); break;
            case '<': g_string_append (s, "&lt;");  break;
            case '>': g_string_append (s, "&gt;");  break;
            default:  g_string_append_c (s, *p);    break;
        }
    }
    return g_string_free (s, FALSE);
}

/* Extract `<tag>...</tag>` blocks (case-insensitive) from `src`,
 * replace each with a saved-placeholder, and append the saved chunk
 * to `ctx->saved`.  `wrap_with` (e.g. "pre") wraps each chunk in
 * <wrap_with>...</wrap_with> before saving; NULL means keep literal.
 * Operates on a copy of src; returns a fresh string. */
static char *
wiki_extract_block (WikiCtx *ctx, const char *src,
                    const char *tag, const char *wrap_with)
{
    GString *out      = g_string_sized_new (strlen (src));
    char    *open_tag = g_strdup_printf ("<%s>", tag);
    char    *close_tag = g_strdup_printf ("</%s>", tag);
    gsize    open_len  = strlen (open_tag);
    gsize    close_len = strlen (close_tag);
    const char *p = src;

    while (*p)
    {
        /* Case-insensitive match for <tag>. */
        if (g_ascii_strncasecmp (p, open_tag, open_len) == 0)
        {
            const char *body  = p + open_len;
            const char *end   = body;
            while (*end && g_ascii_strncasecmp (end, close_tag, close_len) != 0)
                end++;
            if (*end)
            {
                gsize body_len = end - body;
                char *inner    = g_strndup (body, body_len);
                /* HTML-escape inner content before stashing.  <pre>
                 * and <nowiki> bodies bypass the per-line escape
                 * pass, but they still end up in the final HTML
                 * stream; if we don't escape "<q.fang at e.com>"
                 * here libxml2 sees it as an unknown tag and warns.
                 * The "<pre>" wrapper itself stays unescaped so the
                 * block renders as preformatted text. */
                char *escaped  = wiki_quote_html (inner);
                char *to_save;
                char *placeholder;
                if (wrap_with)
                    to_save = g_strdup_printf ("<%s>%s</%s>",
                                               wrap_with, escaped, wrap_with);
                else
                    to_save = g_strdup (escaped);
                placeholder = wiki_save_raw (ctx, to_save);
                g_string_append (out, placeholder);
                g_free (placeholder);
                g_free (to_save);
                g_free (escaped);
                g_free (inner);
                p = end + close_len;
                continue;
            }
        }
        g_string_append_c (out, *p++);
    }

    g_free (open_tag);
    g_free (close_tag);
    return g_string_free (out, FALSE);
}

/* Restore "\x01<N>\x01" placeholders with the stored chunks.  Loops
 * until no more substitutions happen so nested placeholders work. */
static char *
wiki_restore_saved (WikiCtx *ctx, char *text)
{
    GString *out;
    gboolean changed;

    do
    {
        changed = FALSE;
        out = g_string_sized_new (strlen (text));
        for (const char *p = text; *p; )
        {
            if (*p == '\x01')
            {
                const char *q = p + 1;
                guint idx = 0;
                while (*q >= '0' && *q <= '9')
                {
                    idx = idx * 10 + (*q - '0');
                    q++;
                }
                if (*q == '\x01' && idx < ctx->saved->len)
                {
                    g_string_append (out, (const char *) ctx->saved->pdata[idx]);
                    p = q + 1;
                    changed = TRUE;
                    continue;
                }
            }
            g_string_append_c (out, *p++);
        }
        g_free (text);
        text = g_string_free (out, FALSE);
    } while (changed);

    return text;
}

/* Inline rule helpers — these all return a fresh g_strdup'd string. */

/* Replace runs of delim..delim with <open>...</open>.  Non-greedy by
 * matching the nearest closing delim.  delim_len must be ≥1.  Used
 * for ''italic'' / '''bold''' / `code`. */
static char *
wiki_apply_pair (const char *src, const char *delim, gsize delim_len,
                 const char *open_tag, const char *close_tag)
{
    GString    *out = g_string_sized_new (strlen (src));
    const char *p   = src;

    while (*p)
    {
        if (strncmp (p, delim, delim_len) == 0)
        {
            const char *body = p + delim_len;
            const char *end  = strstr (body, delim);
            if (end && end > body)
            {
                g_string_append (out, open_tag);
                g_string_append_len (out, body, end - body);
                g_string_append (out, close_tag);
                p = end + delim_len;
                continue;
            }
        }
        g_string_append_c (out, *p++);
    }
    return g_string_free (out, FALSE);
}

/* Match a URL starting at *p* and return the byte length of the
 * scheme+rest, or 0 if it's not a recognised URL.  Habitat accepts
 * "scheme:" followed by any non-space/bracket characters (so e.g.
 * "http:?Home" and "Local:Page" are valid even without "//"). */
static gsize
wiki_match_url (const char *p)
{
    static const char *schemes[] = {
        "http",  "https", "ftp",   "ftps",   "file",
        "mailto","news",  "nntp",  "gopher", "telnet",
        "wais",  "afs",   "mid",   "cid",    "prospero",
        NULL
    };
    for (int i = 0; schemes[i]; i++)
    {
        gsize slen = strlen (schemes[i]);
        if (g_ascii_strncasecmp (p, schemes[i], slen) == 0
            && p[slen] == ':')
        {
            const char *end = p + slen + 1;
            while (*end && *end != ' ' && *end != '\t' && *end != '\n'
                   && *end != '<' && *end != '>' && *end != '"'
                   && *end != ']' && *end != '|' && *end != '\x01')
                end++;
            /* Trim trailing punctuation that's usually sentence noise. */
            while (end > p + slen + 1
                   && strchr (".,;:)!?", end[-1]))
                end--;
            return end - p;
        }
    }
    return 0;
}

/* WikiWord = uppercase+ lowercase+ uppercase letter+ rest.
 * Returns matched byte length or 0. */
static gsize
wiki_match_wikiword (const char *p)
{
    const char *q = p;
    int upper_run = 0, lower_run = 0;
    while (g_ascii_isupper (*q)) { q++; upper_run++; }
    if (upper_run < 1) return 0;
    while (g_ascii_islower (*q)) { q++; lower_run++; }
    if (lower_run < 1) return 0;
    if (!g_ascii_isupper (*q)) return 0;
    while (g_ascii_isalnum (*q)) q++;
    return q - p;
}

/* Apply all inline rules to `text` (which has already been HTML-escaped).
 * Returns a fresh g_strdup'd string with rich HTML markup interspersed.
 * Links and other "complex" outputs are wrapped in save-raw placeholders
 * so subsequent passes (bold/italic/wikiword) don't re-mangle them. */
static char *
wiki_inline (WikiCtx *ctx, const char *text)
{
    GString    *out = g_string_sized_new (strlen (text) + 32);
    const char *p   = text;

    /* Pass 1: URLs, [[FreeLinks]], [url text] brackets — stash each
     * generated <a> tag in saved/ so later inline passes don't see it.
     * Walk char-by-char looking for the starts of these constructs. */
    while (*p)
    {
        /* Named anchor: [#Anchor] (Habitat NamedAnchors).  Emits an
         * empty <a name="..."> in place so later [[Page#Anchor]] /
         * [url#Anchor] references resolve to it within the rendered
         * preview.  Doesn't show any visible glyph.  Duplicate uses of
         * the same name silently drop the second/third anchor so we
         * don't trip libxml2's "ID already defined" warning. */
        if (p[0] == '[' && p[1] == '#')
        {
            const char *body  = p + 2;
            const char *close = strchr (body, ']');
            if (close && close > body)
            {
                char *name = g_strndup (body, close - body);
                if (!g_hash_table_contains (ctx->anchors_seen, name))
                {
                    char *raw = g_strdup_printf ("<a name=\"%s\"></a>", name);
                    char *ph  = wiki_save_raw (ctx, raw);
                    g_string_append (out, ph);
                    g_free (ph); g_free (raw);
                    g_hash_table_add (ctx->anchors_seen,
                                       g_strdup (name));
                }
                g_free (name);
                p = close + 1;
                continue;
            }
        }

        /* [[Page]] or [[Page|text]] */
        if (p[0] == '[' && p[1] == '[')
        {
            const char *body  = p + 2;
            const char *close = strstr (body, "]]");
            if (close)
            {
                gsize       body_len = close - body;
                char       *content  = g_strndup (body, body_len);
                const char *bar      = strchr (content, '|');
                char       *href, *label, *anchor;
                if (bar)
                {
                    href  = g_strndup (content, bar - content);
                    label = g_strdup (bar + 1);
                }
                else
                {
                    href  = g_strdup (content);
                    label = g_strdup (content);
                }
                anchor = g_strdup_printf ("<a href=\"%s\">%s</a>", href, label);
                {
                    char *ph = wiki_save_raw (ctx, anchor);
                    g_string_append (out, ph);
                    g_free (ph);
                }
                g_free (anchor);
                g_free (href);
                g_free (label);
                g_free (content);
                p = close + 2;
                continue;
            }
        }

        /* [url ...]   or  [url]  */
        if (p[0] == '[')
        {
            gsize ulen = wiki_match_url (p + 1);
            if (ulen > 0)
            {
                const char *url_start = p + 1;
                const char *after_url = url_start + ulen;
                const char *close     = strchr (after_url, ']');
                if (close)
                {
                    char *href = g_strndup (url_start, ulen);
                    const char *text_start = after_url;
                    while (*text_start == ' ' || *text_start == '\t')
                        text_start++;
                    char *anchor;
                    if (text_start < close)
                    {
                        char *label = g_strndup (text_start, close - text_start);
                        anchor = g_strdup_printf ("<a href=\"%s\">%s</a>",
                                                  href, label);
                        g_free (label);
                    }
                    else
                    {
                        ctx->link_counter++;
                        anchor = g_strdup_printf ("<a href=\"%s\">[%d]</a>",
                                                  href, ctx->link_counter);
                    }
                    {
                        char *ph = wiki_save_raw (ctx, anchor);
                        g_string_append (out, ph);
                        g_free (ph);
                    }
                    g_free (anchor);
                    g_free (href);
                    p = close + 1;
                    continue;
                }
            }
        }

        /* Bare URL — must be at word boundary (or start of buffer) */
        if (p == text || !g_ascii_isalnum (p[-1]))
        {
            gsize ulen = wiki_match_url (p);
            if (ulen > 0)
            {
                char *href = g_strndup (p, ulen);
                char *anchor = g_strdup_printf ("<a href=\"%s\">%s</a>",
                                                href, href);
                char *ph = wiki_save_raw (ctx, anchor);
                g_string_append (out, ph);
                g_free (ph);
                g_free (anchor);
                g_free (href);
                p += ulen;
                continue;
            }
        }

        /* WikiWord — must be at word boundary */
        if (p == text || !g_ascii_isalnum (p[-1]))
        {
            gsize wlen = wiki_match_wikiword (p);
            if (wlen > 0)
            {
                char *word   = g_strndup (p, wlen);
                char *anchor = g_strdup_printf ("<a href=\"%s\">%s</a>",
                                                word, word);
                char *ph     = wiki_save_raw (ctx, anchor);
                g_string_append (out, ph);
                g_free (ph);
                g_free (anchor);
                g_free (word);
                p += wlen;
                continue;
            }
        }

        g_string_append_c (out, *p++);
    }

    /* Pass 2: HTML passthrough for the inline tags users actually
     * write inline (Habitat's @HtmlPairs whitelist).  After Phase-2's
     * global HTML-escape these reach us as "&lt;u&gt;text&lt;/u&gt;"
     * etc.; rewrite each pair back to a real tag so MooHtml renders
     * the underline / monospace / etc. visually.  Unknown tags stay
     * escaped (so a stray "<foo>" shows literally rather than being
     * dropped). */
    char *stage = g_string_free (out, FALSE);
    {
        static const char *pairs[] = {
            "b", "i", "u", "s",
            "strong", "em", "strike",
            "code", "tt", "kbd", "var",
            "sub", "sup", "mark", "cite", "q",
            "abbr", "small", "big",
            NULL
        };
        for (int i = 0; pairs[i]; i++)
        {
            char *open_pat  = g_strdup_printf ("&lt;%s&gt;",   pairs[i]);
            char *close_pat = g_strdup_printf ("&lt;/%s&gt;",  pairs[i]);
            char *open_rep  = g_strdup_printf ("<%s>",          pairs[i]);
            char *close_rep = g_strdup_printf ("</%s>",         pairs[i]);

            /* Manual scan: replace open_pat..close_pat with the real
             * tag pair.  Non-greedy match — the nearest close_pat wins.
             * wiki_apply_pair won't do it because it expects the
             * opening and closing delimiters to be identical. */
            GString *next = g_string_sized_new (strlen (stage));
            const char *q = stage;
            gsize open_len  = strlen (open_pat);
            gsize close_len = strlen (close_pat);
            while (*q)
            {
                if (strncmp (q, open_pat, open_len) == 0)
                {
                    const char *body = q + open_len;
                    const char *end  = strstr (body, close_pat);
                    if (end && end > body)
                    {
                        g_string_append (next, open_rep);
                        g_string_append_len (next, body, end - body);
                        g_string_append (next, close_rep);
                        q = end + close_len;
                        continue;
                    }
                }
                g_string_append_c (next, *q++);
            }
            g_free (stage);
            stage = g_string_free (next, FALSE);

            g_free (open_pat); g_free (close_pat);
            g_free (open_rep); g_free (close_rep);
        }
    }

    /* Pass 3: quote markup.  5-quote first (so it doesn't get mangled
     * by the 3- and 2-quote passes), then 3-quote bold, then 2-quote
     * italic, then 1-back-quote inline code.  This is the same
     * 5-then-3-then-2 order Habitat's CommonMarkup achieves with its
     * leading-quote-greedy regex. */
    char *boldit = wiki_apply_pair (stage, "'''''", 5,
                                    "<em><strong>", "</strong></em>");
    g_free (stage);
    char *bold = wiki_apply_pair (boldit, "'''", 3, "<strong>", "</strong>");
    g_free (boldit);
    char *italic = wiki_apply_pair (bold, "''", 2, "<em>", "</em>");
    g_free (bold);
    char *code = wiki_apply_pair (italic, "`", 1, "<code>", "</code>");
    g_free (italic);

    return code;
}

/* Close blocks until depth reaches `target`.  Block stack is FIFO from
 * outermost to innermost; we pop the top until we hit the target. */
static void
wiki_close_to_depth (WikiCtx *ctx, int target)
{
    while (g_slist_length (ctx->blocks) > (guint) target)
    {
        char *tag = (char *) ctx->blocks->data;
        ctx->blocks = g_slist_delete_link (ctx->blocks, ctx->blocks);
        g_string_append_printf (ctx->out, "</%s>\n", tag);
        g_free (tag);
        g_array_remove_index (ctx->block_depth,
                              ctx->block_depth->len - 1);
    }
}

/* Close every open block (used at end of input and on blank lines). */
static void
wiki_close_all_blocks (WikiCtx *ctx)
{
    if (ctx->in_para)
    {
        g_string_append (ctx->out, "</p>\n");
        ctx->in_para = FALSE;
    }
    /* Close any open list/dl first so the inner <li>/<dd> closes
     * before the surrounding non-list block stack. */
    wiki_list_close_to (ctx, 0);
    wiki_close_to_depth (ctx, 0);
}

/* Push a new block (open the tag, push the stack entries). */
static void
wiki_open_block (WikiCtx *ctx, const char *tag, int depth)
{
    g_string_append_printf (ctx->out, "<%s>\n", tag);
    ctx->blocks = g_slist_append (ctx->blocks, g_strdup (tag));
    g_array_append_val (ctx->block_depth, depth);
}

/* wiki_match_list_depth removed — replaced by wiki_list_item /
 * wiki_list_close_to / wiki_list_push_to which produce HTML5-valid
 * nested-list markup (with each inner <ul> wrapped in the parent's
 * <li> rather than as a direct child of the outer <ul>). */

/* Emit a single line's content, with paragraph wrapping and inline
 * rules applied.  Caller has already classified the line as plain
 * paragraph text. */
static void
wiki_emit_paragraph_line (WikiCtx *ctx, const char *line)
{
    char *inlined = wiki_inline (ctx, line);
    if (!ctx->in_para)
    {
        g_string_append (ctx->out, "<p>");
        ctx->in_para = TRUE;
    }
    else
    {
        g_string_append (ctx->out, " ");
    }
    g_string_append (ctx->out, inlined);
    g_free (inlined);
}

/* Process one line.  Caller passes the HTML-escaped line text (without
 * trailing newline).  Determines block type, manages the stack, emits
 * the line's content. */
static void
wiki_process_line (WikiCtx *ctx, const char *line)
{
    /* Blank line — end paragraph, close transient blocks (pre/table).
     * Lists/dl stay open across blank-line gaps? Habitat closes them;
     * we close everything for simplicity. */
    if (line[0] == '\0')
    {
        wiki_close_all_blocks (ctx);
        return;
    }

    /* ---- <toc> marker on its own line — handled as a block so it
     * doesn't get wrapped in <p> (which would make libxml2 reject the
     * generated nested-list TOC).  After Phase 2 the marker reaches
     * us as "&lt;toc&gt;"; replace it with a sentinel that survives
     * the rest of the pipeline and gets expanded in Phase 5. */
    {
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (g_ascii_strncasecmp (q, "&lt;toc&gt;", 11) == 0)
        {
            const char *tail = q + 11;
            while (*tail == ' ' || *tail == '\t') tail++;
            if (*tail == '\0')
            {
                wiki_close_all_blocks (ctx);
                g_string_append (ctx->out, "\x01TOC\x01\n");
                return;
            }
        }
    }

    /* ---- Block-level placeholder: a line that's just
     * \x01<digits>\x01 (optionally surrounded by whitespace) is a
     * stashed protected-region marker (<pre>/<nowiki>/<code>) from
     * Phase 1.  When restored in Phase 4 the content typically has
     * its own block-level tag (e.g. <pre>...</pre>), so we mustn't
     * wrap it in <p> — otherwise libxml2 reports "<pre> inside <p>"
     * and closes the paragraph prematurely.  Emit on its own line at
     * block level. */
    {
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '\x01')
        {
            const char *r = q + 1;
            while (*r >= '0' && *r <= '9') r++;
            if (r > q + 1 && *r == '\x01')
            {
                const char *tail = r + 1;
                while (*tail == ' ' || *tail == '\t') tail++;
                if (*tail == '\0')
                {
                    wiki_close_all_blocks (ctx);
                    g_string_append_len (ctx->out, q, r + 1 - q);
                    g_string_append_c (ctx->out, '\n');
                    return;
                }
            }
        }
    }

    /* ---- Horizontal rule ----
     * Habitat: 4+ dashes = thin rule, 6+ = thick rule.  Trim trailing
     * whitespace before counting (a stray space after the dashes is
     * common and shouldn't break detection). */
    if (line[0] == '-' && line[1] == '-' && line[2] == '-' && line[3] == '-')
    {
        gsize       len      = strlen (line);
        const char *trim_end = line + len;
        while (trim_end > line && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
            trim_end--;
        gboolean all_dash = (trim_end > line);
        for (const char *p = line; p < trim_end; p++)
            if (*p != '-') { all_dash = FALSE; break; }
        if (all_dash)
        {
            int dash_count = (int) (trim_end - line);
            wiki_close_all_blocks (ctx);
            if (dash_count >= 6)
                g_string_append (ctx->out, "<hr class=\"wikiline-thick\">\n");
            else
                g_string_append (ctx->out, "<hr>\n");
            return;
        }
    }

    /* ---- Heading: = title = up to ====== title ====== ---- */
    if (line[0] == '=')
    {
        int level = 0;
        const char *p = line;
        while (*p == '=' && level < 6) { p++; level++; }
        if (level >= 1 && *p == ' ')
        {
            /* Find the matching =s at the tail */
            const char *end = line + strlen (line);
            const char *tail = end;
            while (tail > p && tail[-1] == ' ') tail--;
            int tail_eq = 0;
            while (tail > p && tail[-1] == '=' && tail_eq < level)
            {
                tail--;
                tail_eq++;
            }
            if (tail_eq == level && tail > p + 1)
            {
                wiki_close_all_blocks (ctx);
                /* Skip leading space after opening = */
                const char *title_start = p + 1;
                /* Trim trailing spaces */
                const char *title_end = tail;
                while (title_end > title_start
                       && title_end[-1] == ' ')
                    title_end--;
                char *title = g_strndup (title_start,
                                          title_end - title_start);

                /* Habitat convention: a leading "#" after the opening
                 * equals signs ("== # Heading ==") marks this heading
                 * as TOC-worthy AND creates a named anchor for it.
                 * Strip the "#" + spaces from the display title, and
                 * record an entry in ctx->toc that will be replayed
                 * when wiki_to_html post-processes <toc> markers. */
                gboolean in_toc = FALSE;
                char *display = title;
                if (display[0] == '#'
                    && (display[1] == ' ' || display[1] == '\t'
                        || display[1] == '\0'))
                {
                    in_toc = TRUE;
                    display = title + 1;
                    while (*display == ' ' || *display == '\t')
                        display++;
                }

                char *anchor = NULL;
                char *number = NULL;   /* "1.2.3" prefix for == # … == */
                if (in_toc)
                {
                    anchor = wiki_slugify (display);
                    if (!anchor)
                        anchor = g_strdup_printf ("section-%d",
                                                   ctx->anchor_seq);
                    ctx->anchor_seq++;

                    /* Build hierarchical section number.  Bump our
                     * level's counter, reset deeper levels, then
                     * format counters[1..level] skipping leading zeros
                     * (so a document that starts at H2 still numbers
                     * "1." rather than "0.1."). */
                    ctx->heading_counters[level]++;
                    for (int j = level + 1; j <= 6; j++)
                        ctx->heading_counters[j] = 0;
                    GString *numbuf = g_string_new (NULL);
                    gboolean started = FALSE;
                    for (int j = 1; j <= level; j++)
                    {
                        if (!started && ctx->heading_counters[j] == 0)
                            continue;
                        g_string_append_printf (numbuf,
                            started ? ".%d" : "%d",
                            ctx->heading_counters[j]);
                        started = TRUE;
                    }
                    if (numbuf->len == 0)
                        g_string_append_c (numbuf, '1');
                    number = g_string_free (numbuf, FALSE);

                    WikiTocEntry *e = g_new0 (WikiTocEntry, 1);
                    e->level  = level;
                    e->title  = g_strdup (display);
                    e->anchor = g_strdup (anchor);
                    e->number = g_strdup (number);
                    g_ptr_array_add (ctx->toc, e);
                }

                char *inlined = wiki_inline (ctx, display);
                /* Prefix the rendered heading text with the section
                 * number plus a trailing dot so the body shows
                 * "1.2. Title" in front of the H2/H3 chrome.  Mirrors
                 * Habitat's TOC + heading numbering style. */
                char *heading_text = number
                    ? g_strdup_printf ("%s. %s", number, inlined)
                    : g_strdup (inlined);

                if (anchor)
                {
                    /* Skip the <a name> if a previous heading or
                     * inline [#anchor] already registered this name. */
                    if (g_hash_table_contains (ctx->anchors_seen, anchor))
                    {
                        g_string_append_printf (ctx->out,
                            "<h%d>%s</h%d>\n",
                            level, heading_text, level);
                    }
                    else
                    {
                        g_string_append_printf (ctx->out,
                            "<h%d><a name=\"%s\"></a>%s</h%d>\n",
                            level, anchor, heading_text, level);
                        g_hash_table_add (ctx->anchors_seen,
                                           g_strdup (anchor));
                    }
                }
                else
                    g_string_append_printf (ctx->out, "<h%d>%s</h%d>\n",
                                             level, heading_text, level);
                g_free (heading_text);
                g_free (inlined);
                g_free (anchor);
                g_free (number);
                g_free (title);
                return;
            }
        }
    }

    /* ---- Unordered list (*) and ordered list (#) ---- */
    if (line[0] == '*' || line[0] == '#')
    {
        char ch    = line[0];
        int  depth = 0;
        const char *p = line;
        while (*p == ch) { p++; depth++; }
        if (depth >= 1 && (*p == ' ' || *p == '\t' || *p == '\0'))
        {
            const char *content = p;
            while (*content == ' ' || *content == '\t') content++;
            if (ctx->in_para) {
                g_string_append (ctx->out, "</p>\n");
                ctx->in_para = FALSE;
            }
            char *inlined = wiki_inline (ctx, content);
            wiki_list_item (ctx, ch == '*' ? 'u' : 'o', depth,
                            "<li>", inlined);
            g_free (inlined);
            return;
        }
    }

    /* ---- Definition list: ; term : def ---- */
    if (line[0] == ';')
    {
        int depth = 0;
        const char *p = line;
        while (*p == ';') { p++; depth++; }
        const char *colon = strchr (p, ':');
        if (colon)
        {
            char *term = g_strndup (p, colon - p);
            char *def  = g_strdup (colon + 1);
            char *t    = wiki_inline (ctx, g_strstrip (term));
            char *d    = wiki_inline (ctx, g_strstrip (def));
            if (ctx->in_para) {
                g_string_append (ctx->out, "</p>\n");
                ctx->in_para = FALSE;
            }
            char *combined = g_strdup_printf ("<dt>%s</dt><dd>%s", t, d);
            wiki_list_item (ctx, 'd', depth, "", combined);
            g_free (combined);
            g_free (t); g_free (d); g_free (term); g_free (def);
            return;
        }
    }

    /* ---- Indented (: text) ---- */
    if (line[0] == ':')
    {
        int depth = 0;
        const char *p = line;
        while (*p == ':') { p++; depth++; }
        char *inlined = wiki_inline (ctx, p);
        if (ctx->in_para) {
            g_string_append (ctx->out, "</p>\n");
            ctx->in_para = FALSE;
        }
        wiki_list_item (ctx, 'd', depth, "<dd>", inlined);
        g_free (inlined);
        return;
    }

    /* ---- Table row: ||cell||cell||  or  !!cell!!cell!! ---- */
    if ((line[0] == '|' && line[1] == '|')
        || (line[0] == '!' && line[1] == '!'))
    {
        char         sep_char = line[0];          /* '|' or '!'         */
        const char  *cell_tag = (sep_char == '|') ? "td" : "th";

        if (ctx->in_para) {
            g_string_append (ctx->out, "</p>\n");
            ctx->in_para = FALSE;
        }
        if (wiki_in_list (ctx))
            wiki_list_close_to (ctx, 0);
        /* Open <table> if not already in one */
        if (!ctx->blocks || strcmp ((char *) ctx->blocks->data, "table") != 0)
        {
            wiki_close_all_blocks (ctx);
            wiki_open_block (ctx, "table", 1);
        }

        /* Parse the row from left to right.  Each cell is preceded by
         * a run of `sep_char` pairs; the run length divided by 2 is
         * the colspan for the next cell.  An underscore run "_+"
         * right after the cell-separator gives rowspan.  Habitat's
         * regex from Render.pm:
         *   ((\|\|)+)(\_*) → colspan = length($1)/2,
         *                    rowspan = length($3)
         */
        const char *p = line;
        gboolean    first_cell = TRUE;
        g_string_append (ctx->out, "<tr>");
        while (*p)
        {
            /* Consume the run of sep_char pairs.  Count pairs. */
            int pair_count = 0;
            while (p[0] == sep_char && p[1] == sep_char)
            {
                pair_count++;
                p += 2;
            }
            if (pair_count == 0)
                break;        /* not at a separator — malformed row */

            /* Underscore prefix on the next cell → rowspan. */
            int rowspan = 0;
            while (*p == '_')
            {
                rowspan++;
                p++;
            }

            /* The cell content runs until the next run of sep_char
             * pairs (or end-of-line). */
            const char *cell_start = p;
            while (*p)
            {
                if (p[0] == sep_char && p[1] == sep_char)
                    break;
                p++;
            }
            const char *cell_end = p;

            /* The pair count we just consumed describes the cell
             * AFTER it (the one we're about to emit), unless we're at
             * the very start of the line — in that case the pair
             * count of the leading "||" is just the row marker and a
             * colspan>1 still belongs to the first cell.  This matches
             * Habitat's semantics. */
            int colspan = pair_count;     /* default: the consumed run */
            if (first_cell && colspan == 1)
                colspan = 1;
            first_cell = FALSE;

            /* If we're at end-of-line (cell_start == cell_end and
             * there's nothing after), this is the terminal "||" — no
             * cell to emit. */
            if (cell_start == cell_end && *cell_end == '\0')
                break;

            char *cell    = g_strndup (cell_start, cell_end - cell_start);
            char *trimmed = g_strstrip (cell);
            char *inlined = wiki_inline (ctx, trimmed);
            g_string_append_printf (ctx->out, "<%s", cell_tag);
            if (colspan > 1)
                g_string_append_printf (ctx->out, " colspan=\"%d\"",
                                         colspan);
            if (rowspan > 1)
                g_string_append_printf (ctx->out, " rowspan=\"%d\"",
                                         rowspan);
            g_string_append_printf (ctx->out, ">%s</%s>",
                                     inlined, cell_tag);
            g_free (inlined);
            g_free (cell);
        }
        g_string_append (ctx->out, "</tr>\n");
        return;
    }

    /* ---- Preformatted (leading space or tab) ---- */
    if (line[0] == ' ' || line[0] == '\t')
    {
        if (ctx->in_para) {
            g_string_append (ctx->out, "</p>\n");
            ctx->in_para = FALSE;
        }
        if (!ctx->blocks || strcmp ((char *) ctx->blocks->data, "pre") != 0)
        {
            wiki_close_all_blocks (ctx);
            wiki_open_block (ctx, "pre", 1);
        }
        g_string_append (ctx->out, line);
        g_string_append_c (ctx->out, '\n');
        return;
    }

    /* ---- Default: paragraph text ---- */
    /* If we were in a list or other non-paragraph block, close it
     * first so a paragraph after a list doesn't get adopted into it. */
    if (wiki_in_list (ctx))
        wiki_list_close_to (ctx, 0);
    if (ctx->blocks && strcmp ((char *) ctx->blocks->data, "pre") == 0)
        wiki_close_all_blocks (ctx);
    if (ctx->blocks
        && (strcmp ((char *) ctx->blocks->data, "table") == 0))
        wiki_close_all_blocks (ctx);
    wiki_emit_paragraph_line (ctx, line);
}

/* Top-level converter.  Returns g_strdup'd HTML (caller frees). */
static char *
wiki_to_html (const char *src)
{
    WikiCtx ctx = { 0 };
    ctx.out          = g_string_sized_new (strlen (src) * 2 + 64);
    ctx.saved        = g_ptr_array_new_with_free_func (g_free);
    ctx.blocks       = NULL;
    ctx.block_depth  = g_array_new (FALSE, FALSE, sizeof (int));
    ctx.in_para      = FALSE;
    ctx.link_counter = 0;
    ctx.toc          = g_ptr_array_new_with_free_func (wiki_toc_entry_free);
    ctx.anchor_seq   = 1;
    ctx.anchors_seen = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, NULL);
    ctx.list_stack   = g_array_new (FALSE, FALSE, sizeof (char));
    ctx.list_li_open = g_array_new (FALSE, FALSE, sizeof (gboolean));

    /* Phase 1: pull out protected regions BEFORE HTML-escaping.  These
     * are the only places where we honour literal HTML; everything else
     * goes through wiki_quote_html. */
    char *step1 = wiki_extract_block (&ctx, src, "nowiki", NULL);
    char *step2 = wiki_extract_block (&ctx, step1, "pre", "pre");
    g_free (step1);
    char *step3 = wiki_extract_block (&ctx, step2, "code", "pre");
    g_free (step2);

    /* Phase 2: HTML-escape everything else.  Placeholders are pure ASCII
     * (\x01<digits>\x01) so they survive quoting. */
    char *escaped = wiki_quote_html (step3);
    g_free (step3);

    /* Phase 2.5: line continuation — backslash at end of line joins
     * with the next line as a single logical line.  Done in-place on
     * the escaped buffer before the line split. */
    {
        GString *joined = g_string_sized_new (strlen (escaped));
        for (const char *p = escaped; *p; p++)
        {
            if (*p == '\\' && p[1] == '\n')
            {
                g_string_append_c (joined, ' ');
                p++;       /* swallow the newline */
            }
            else
            {
                g_string_append_c (joined, *p);
            }
        }
        g_free (escaped);
        escaped = g_string_free (joined, FALSE);
    }

    /* Phase 3: line-by-line block processing. */
    char **lines = g_strsplit (escaped, "\n", -1);
    g_free (escaped);
    for (char **l = lines; *l; l++)
        wiki_process_line (&ctx, *l);
    g_strfreev (lines);

    wiki_close_all_blocks (&ctx);

    /* Phase 4: restore protected regions. */
    char *result = g_string_free (ctx.out, FALSE);
    result = wiki_restore_saved (&ctx, result);

    /* Phase 5: build the TOC and splice it in at the \x01TOC\x01
     * sentinels left behind by the line processor.  Nested-list HTML
     * has to wrap inner <ul>s inside the previous <li> (HTML5 doesn't
     * accept <ul> as a direct child of <ul>), which makes building it
     * from a flat (level, title) list mildly awkward — see the
     * deferred-close state machine below. */
    {
        GString *toc = g_string_new (NULL);
        if (ctx.toc->len == 0)
        {
            g_string_append (toc,
                "<div class=\"wiki-toc\"><em>"
                "(no headings marked for the table of contents — use "
                "<code>== # Title ==</code>)</em></div>\n");
        }
        else
        {
            /* Find the shallowest heading level used by the document so
             * we can shift it to TOC depth 1 — keeps a document that
             * starts at H2 from showing a dummy "level-1" bullet above
             * every entry.  Heading numbers were already computed at
             * emit time using the raw levels, and we preserve those
             * as-is regardless of the depth normalization. */
            int min_level = 6;
            for (guint i = 0; i < ctx.toc->len; i++)
            {
                WikiTocEntry *e = (WikiTocEntry *) ctx.toc->pdata[i];
                if (e->level < min_level) min_level = e->level;
            }
            if (min_level < 1) min_level = 1;

            int      depth   = 0;
            gboolean li_open = FALSE;

            g_string_append (toc,
                "<div class=\"wiki-toc\"><b>Contents</b>\n");
            for (guint i = 0; i < ctx.toc->len; i++)
            {
                WikiTocEntry *e = (WikiTocEntry *) ctx.toc->pdata[i];
                int eff_level = e->level - min_level + 1;
                if (eff_level < 1) eff_level = 1;
                if (eff_level > 6) eff_level = 6;

                /* Pop deeper levels. */
                while (depth > eff_level)
                {
                    if (li_open)
                    {
                        g_string_append (toc, "</li>\n");
                        li_open = FALSE;
                    }
                    g_string_append (toc, "</ul>\n");
                    depth--;
                    if (depth >= eff_level)
                        g_string_append (toc, "</li>\n");
                }

                /* Push to entry's effective level. */
                while (depth < eff_level)
                {
                    if (li_open)
                    {
                        g_string_append (toc, "<ul>\n");
                        li_open = FALSE;
                    }
                    else if (depth == 0)
                        g_string_append (toc, "<ul>\n");
                    else
                        g_string_append (toc, "<li><ul>\n");
                    depth++;
                }

                /* Close previous sibling <li> at this level. */
                if (li_open)
                    g_string_append (toc, "</li>\n");
                g_string_append_printf (toc,
                    "<li><a href=\"#%s\">%s%s %s</a>",
                    e->anchor,
                    e->number ? e->number : "",
                    e->number ? "." : "",
                    e->title);
                li_open = TRUE;
            }
            if (li_open)
                g_string_append (toc, "</li>\n");
            while (depth > 0)
            {
                g_string_append (toc, "</ul>\n");
                depth--;
                if (depth > 0)
                    g_string_append (toc, "</li>\n");
            }
            g_string_append (toc, "</div>\n");
        }

        /* Replace each "\x01TOC\x01" sentinel with the rendered TOC.
         * The sentinel was emitted as a block (not inside <p>) so the
         * inner <div>/<ul> aren't trapped in an inline context. */
        GString *out2 = g_string_sized_new (strlen (result) + toc->len);
        for (const char *p = result; *p; )
        {
            if (strncmp (p, "\x01TOC\x01", 5) == 0)
            {
                g_string_append (out2, toc->str);
                p += 5;
            }
            else
                g_string_append_c (out2, *p++);
        }
        g_string_free (toc, TRUE);
        g_free (result);
        result = g_string_free (out2, FALSE);
    }

    g_ptr_array_free (ctx.saved, TRUE);
    g_ptr_array_free (ctx.toc, TRUE);
    g_hash_table_destroy (ctx.anchors_seen);
    g_array_free (ctx.block_depth, TRUE);
    g_array_free (ctx.list_stack, TRUE);
    g_array_free (ctx.list_li_open, TRUE);
    g_slist_free_full (ctx.blocks, g_free);
    return result;
}
#endif /* MOO_BUILD_WIKI */

/* Markdown-specific tag styling — applied after every MooHtml load so
 * the Markdown preview pane has its own visual identity (theme-aware
 * link colour, subtle code/pre backgrounds, slightly coloured headings)
 * without touching how the About dialog renders.
 *
 * MooHtml's tags are mostly anonymous; we identify each kind by the
 * narrow predicates added to moohtml.h.  Theme awareness reuses the
 * same luma-based picker we already use for spell-check underlines. */
typedef struct {
    GdkRGBA link;
    GdkRGBA code_bg;       /* background for inline <code> and <pre>   */
    GdkRGBA code_fg;       /* foreground for code (a touch dimmer)     */
    GdkRGBA heading_fg;    /* shared colour for H1/H2 (others: theme)  */
    GdkRGBA hr_fg;
    GdkRGBA quote_bg;      /* soft background for <blockquote>         */
    GdkRGBA quote_fg;      /* dimmer text inside <blockquote>          */
    GdkRGBA table_bg;      /* soft background for tables               */
} PagePreviewPalette;

static void
page_preview_pick_palette (GtkWidget *html_view, PagePreviewPalette *p)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context (html_view);
    GdkRGBA bg = { 1.0, 1.0, 1.0, 1.0 };
    double  luma;

    gtk_style_context_save (ctx);
    gtk_style_context_add_class (ctx, GTK_STYLE_CLASS_VIEW);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_style_context_get_background_color (ctx,
        gtk_style_context_get_state (ctx), &bg);
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_style_context_restore (ctx);

    luma = 0.299 * bg.red + 0.587 * bg.green + 0.114 * bg.blue;
    if (luma <= 0.5)
    {
        /* Dark theme: punchy accents on a near-black background. */
        gdk_rgba_parse (&p->link,       "#7eb6ff");   /* light blue */
        gdk_rgba_parse (&p->code_bg,    "#2b2b2b");   /* slightly lighter than bg */
        gdk_rgba_parse (&p->code_fg,    "#e6e6e6");
        gdk_rgba_parse (&p->heading_fg, "#9cdcfe");   /* cyan-blue (VS-Code-ish) */
        gdk_rgba_parse (&p->hr_fg,      "#444444");
        gdk_rgba_parse (&p->quote_bg,   "#1e2227");   /* faint panel tint */
        gdk_rgba_parse (&p->quote_fg,   "#a8b1bd");   /* muted grey-blue */
        gdk_rgba_parse (&p->table_bg,   "#1e2227");   /* same as quote */
    }
    else
    {
        /* Light theme: deeper colours so they read on white. */
        gdk_rgba_parse (&p->link,       "#1a73e8");   /* Google-style blue */
        gdk_rgba_parse (&p->code_bg,    "#f5f5f5");   /* very light grey */
        gdk_rgba_parse (&p->code_fg,    "#222222");
        gdk_rgba_parse (&p->heading_fg, "#1a1a1a");   /* near-black, lets size carry */
        gdk_rgba_parse (&p->hr_fg,      "#cccccc");
        gdk_rgba_parse (&p->quote_bg,   "#f6f8fa");   /* GitHub-ish faint panel */
        gdk_rgba_parse (&p->quote_fg,   "#586069");   /* GitHub-ish muted text */
        gdk_rgba_parse (&p->table_bg,   "#fafbfc");   /* even softer than quote */
    }
}

/* Iterate every tag in the preview buffer's table and override its
 * properties based on its element kind.  Idempotent — every render
 * call rebuilds the buffer, so this just paints over MooHtml's
 * fresh tags.  Called from page_preview_render() right after the load. */
static void
page_preview_restyle_tags (GtkWidget *html_view)
{
    GtkTextBuffer   *buf   = gtk_text_view_get_buffer (GTK_TEXT_VIEW (html_view));
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table (buf);
    PagePreviewPalette  pal;

    page_preview_pick_palette (html_view, &pal);

    gtk_text_tag_table_foreach (
        table,
        [](GtkTextTag *tag, gpointer data) {
            const PagePreviewPalette *p = (const PagePreviewPalette *) data;
            int h;

            if (_moo_html_tag_is_link (tag))
            {
                /* High-contrast underlined link — matches the GitHub
                 * preview, where every link gets a single line below. */
                g_object_set (tag,
                              "foreground-rgba", &p->link,
                              "underline",       PANGO_UNDERLINE_SINGLE,
                              (const char*) NULL);
            }

            if ((h = _moo_html_tag_get_heading (tag)) > 0)
            {
                /* Lift H1/H2 with a coloured tint; H3+ rely on size +
                 * weight alone to avoid a rainbow effect. */
                if (h <= 2)
                    g_object_set (tag, "foreground-rgba", &p->heading_fg, (const char*) NULL);
            }

            if (_moo_html_tag_is_pre (tag))
            {
                /* <pre> block — tight line-to-line spacing inside the
                 * code block (pixels-above/below-lines apply to *every*
                 * line, so anything > a couple of px adds a visible gap
                 * between consecutive code lines).  The block-edge
                 * padding above and below the whole block comes from
                 * the blank lines around <pre> in the parsed HTML. */
                g_object_set (tag,
                              "paragraph-background-rgba", &p->code_bg,
                              "foreground-rgba",           &p->code_fg,
                              "left-margin",                6,
                              "right-margin",               6,
                              "pixels-above-lines",         1,
                              "pixels-below-lines",         1,
                              "pixels-inside-wrap",         0,
                              "scale",                      0.92,
                              (const char*) NULL);
            }
            else if (_moo_html_tag_is_table (tag))
            {
                /* Tables render as column-aligned monospace text with a
                 * thin under-header rule; the faint background plus a
                 * left indent make the block read as a tabular unit. */
                g_object_set (tag,
                              "paragraph-background-rgba", &p->table_bg,
                              "left-margin",               16,
                              "right-margin",              16,
                              "scale",                     0.92,
                              "pixels-above-lines",        2,
                              "pixels-below-lines",        2,
                              (const char*) NULL);
            }
            else if (_moo_html_tag_is_monospace (tag))
            {
                /* Inline <code> — span-background only (not whole para). */
                g_object_set (tag,
                              "background-rgba", &p->code_bg,
                              "foreground-rgba", &p->code_fg,
                              "scale",           0.92,
                              (const char*) NULL);
            }

            if (_moo_html_tag_is_blockquote (tag))
            {
                /* GitHub-style: indented, italic, muted text on a faint
                 * paragraph background.  left-margin already came from
                 * the MOO_HTML_LEFT_MARGIN flag in the tag's attr. */
                g_object_set (tag,
                              "paragraph-background-rgba", &p->quote_bg,
                              "foreground-rgba",           &p->quote_fg,
                              "style",                     PANGO_STYLE_ITALIC,
                              "pixels-above-lines",        4,
                              "pixels-below-lines",        4,
                              (const char*) NULL);
            }
        },
        &pal);
}

/* Per-buffer renderer kind, picked once per render call. */
typedef enum {
    PREVIEW_NONE,
    PREVIEW_MARKDOWN,
    PREVIEW_WIKI
} PreviewKind;

/* Decide whether `view`'s buffer should be rendered as Markdown, Wiki,
 * or neither.  Two sources, in priority order:
 *   1. First-line marker  <!-- markdown -->  or  <!-- wiki -->
 *   2. Filename extension (.md/.markdown/.mdx → md, .wiki/.wp/.usemod → wiki)
 * Anything else returns PREVIEW_NONE so the pane shows an
 * informational placeholder rather than misrendering arbitrary text. */
static PreviewKind
detect_preview_type (MooEditView *view, const char *first_line)
{
    /* 1. First-line marker beats everything (lets users override the
     *    detection for e.g. a .txt file containing markdown). */
    if (first_line)
    {
        /* Skip leading whitespace. */
        const char *p = first_line;
        while (*p == ' ' || *p == '\t') p++;
        if (g_ascii_strncasecmp (p, "<!--", 4) == 0)
        {
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
            if (g_ascii_strncasecmp (p, "markdown", 8) == 0)
                return PREVIEW_MARKDOWN;
            if (g_ascii_strncasecmp (p, "wiki", 4) == 0)
                return PREVIEW_WIKI;
        }
    }

    /* 2. Filename extension. */
    MooEdit *doc = view ? moo_edit_view_get_doc (view) : NULL;
    if (doc)
    {
        char *filename = moo_edit_get_filename (doc);
        if (filename)
        {
            const char *base = strrchr (filename, '/');
            base = base ? base + 1 : filename;
            const char *ext = strrchr (base, '.');
            PreviewKind kind = PREVIEW_NONE;
            if (ext)
            {
                if (g_ascii_strcasecmp (ext, ".md") == 0
                    || g_ascii_strcasecmp (ext, ".markdown") == 0
                    || g_ascii_strcasecmp (ext, ".mdx") == 0)
                    kind = PREVIEW_MARKDOWN;
                else if (g_ascii_strcasecmp (ext, ".wiki") == 0
                         || g_ascii_strcasecmp (ext, ".wp") == 0
                         || g_ascii_strcasecmp (ext, ".usemod") == 0
                         /* .txt is the de-facto Habitat convention —
                          * plain-text README / NEWS / CHANGELOG files
                          * often use the wiki dialect.  Users who'd
                          * rather see the placeholder can drop a
                          * <!-- markdown --> marker on line 1 (or
                          * leave the pane hidden). */
                         || g_ascii_strcasecmp (ext, ".txt") == 0)
                    kind = PREVIEW_WIKI;
            }
            g_free (filename);
            return kind;
        }
    }

    return PREVIEW_NONE;
}

/* Set the pane's body to an informational placeholder when there's
 * nothing to render.  The placeholder is plain HTML so MooHtml's
 * existing styling handles it for free. */
static void
page_preview_set_placeholder (PagePreviewWindowPlugin *plugin,
                              const char              *message)
{
    char *html = g_strdup_printf (
        "<html><body><p><i>%s</i></p></body></html>", message);
    _moo_html_load_memory (GTK_TEXT_VIEW (plugin->html_view),
                           html, strlen (html), NULL, "UTF-8");
    g_free (html);
}

/* Convert the active document's buffer text to HTML using whichever
 * renderer matches the document type (Markdown via md4c, or Wiki via
 * the in-tree converter), then load it into the MooHtml widget.
 * No-op when there's no active doc or the pane was never built. */
static void
page_preview_render (PagePreviewWindowPlugin *plugin, MooEditView *view_hint)
{
    MooEditWindow *window;
    MooEditView   *view;
    GtkTextBuffer *buffer;
    GtkTextIter    start, end;
    char          *text;
    PreviewKind    kind;

    if (plugin == NULL || plugin->html_view == NULL)
        return;

    window = MOO_WIN_PLUGIN (plugin)->window;
    view   = view_hint ? view_hint : moo_edit_window_get_active_view (window);
    if (view == NULL)
        return;

    buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    gtk_text_buffer_get_bounds (buffer, &start, &end);
    text = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
    if (text == NULL)
        return;

    /* Sniff the first line for an in-document marker. */
    const char *nl = strchr (text, '\n');
    char *first_line = nl ? g_strndup (text, nl - text) : g_strdup (text);
    kind = detect_preview_type (view, first_line);
    g_free (first_line);

    if (kind == PREVIEW_NONE)
    {
        page_preview_set_placeholder (plugin,
            "No preview for this file type.&nbsp; "
            "Open a .md, .markdown, .wiki, or .wp file, "
            "or start the document with "
            "&lt;!-- markdown --&gt; / &lt;!-- wiki --&gt; to force a renderer.");
        g_free (text);
        return;
    }

    GString *html = g_string_sized_new (strlen (text) * 2 + 64);
    g_string_append (html, "<html><body>");

    if (kind == PREVIEW_MARKDOWN)
    {
#ifdef MOO_BUILD_MARKDOWN
        /* md4c writes only the <body> contents; we already provided
         * the envelope.  MD_DIALECT_GITHUB enables tables,
         * strikethrough, task-lists and autolinks. */
        /* MD_DIALECT_GITHUB enables tables/strikethrough/task lists/
         * autolinks.  ADD MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS
         * to disable raw HTML passthrough — real-world README files
         * often contain things like "<param1>" or "<q.fang at e.com>"
         * which md4c would otherwise emit verbatim, leaving libxml2
         * to choke on the resulting unbalanced tags.  Note: this also
         * disables intentional inline HTML; the preview is just a
         * preview, so the trade-off is worth it. */
        unsigned md_flags = MD_DIALECT_GITHUB
                            | MD_FLAG_NOHTMLBLOCKS
                            | MD_FLAG_NOHTMLSPANS;
        int rc = md_html (text, (MD_SIZE) strlen (text),
                          md4c_output_cb, html,
                          md_flags, 0);
        if (rc != 0)
        {
            g_string_truncate (html, 0);
            g_string_append (html,
                "<html><body><p><i>Markdown rendering failed.</i></p></body></html>");
        }
        else
        {
            g_string_append (html, "</body></html>");
        }
#else
        g_string_truncate (html, 0);
        g_string_append (html,
            "<html><body><p><i>Markdown preview not available — "
            "medit was built without md4c.</i></p></body></html>");
#endif
    }
    else /* PREVIEW_WIKI */
    {
#ifdef MOO_BUILD_WIKI
        char *wiki_html = wiki_to_html (text);
        g_string_append (html, wiki_html);
        g_free (wiki_html);
        g_string_append (html, "</body></html>");
#else
        g_string_truncate (html, 0);
        g_string_append (html,
            "<html><body><p><i>Wiki preview not available — "
            "medit was built without MOO_BUILD_WIKI.</i></p></body></html>");
#endif
    }

    /* Optional debug dump — set MOO_PAGE_PREVIEW_DUMP_HTML to a
     * filename to capture the post-conversion HTML.  Useful when
     * tracking down rendering surprises (mismatched tags, missing
     * <hr>, etc.) without rebuilding the renderer. */
    const char *dump_path = g_getenv ("MOO_PAGE_PREVIEW_DUMP_HTML");
    if (dump_path && *dump_path)
        g_file_set_contents (dump_path, html->str, html->len, NULL);

    _moo_html_load_memory (GTK_TEXT_VIEW (plugin->html_view),
                           html->str, html->len, NULL, "UTF-8");
    /* The palette restyle is renderer-agnostic — same theme-aware
     * colours and per-tag overrides apply to both md and wiki output. */
    page_preview_restyle_tags (plugin->html_view);

    g_string_free (html, TRUE);
    g_free (text);
}

/* Debounce-timer callback.  Render once and clear the slot so the next
 * "buffer changed" can arm a fresh timer. */
static gboolean
page_preview_render_timeout (gpointer data)
{
    PagePreviewWindowPlugin *plugin = (PagePreviewWindowPlugin *) data;
    plugin->render_timeout_id = 0;
    page_preview_render (plugin, NULL);
    return G_SOURCE_REMOVE;
}

/* GtkTextBuffer "changed" signal handler.  Reset the debounce timer
 * so multiple rapid edits coalesce into a single render once the user
 * pauses. */
static void
page_preview_on_buffer_changed (G_GNUC_UNUSED GtkTextBuffer *buffer,
                            gpointer                     data)
{
    PagePreviewWindowPlugin *plugin = (PagePreviewWindowPlugin *) data;
    if (plugin->render_timeout_id != 0)
        g_source_remove (plugin->render_timeout_id);
    plugin->render_timeout_id =
        g_timeout_add (PAGE_PREVIEW_DEBOUNCE_MS, page_preview_render_timeout, plugin);
}

/* (Re-)wire the "changed" handler onto whichever buffer is current for
 * the window's active doc.  Tolerates NULL doc (just disconnects). */
static void
page_preview_rewire_buffer_signal (PagePreviewWindowPlugin *plugin)
{
    MooEditWindow *window = MOO_WIN_PLUGIN (plugin)->window;
    MooEditView   *view;
    GtkTextBuffer *buffer = NULL;

    view = moo_edit_window_get_active_view (window);
    if (view != NULL)
        buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));

    if (buffer == plugin->current_buffer)
        return;     /* nothing to do — already wired to this buffer */

    /* A reference is held while wired up: the previewed document can be
       closed at any time, and disconnecting the handler afterwards would
       touch a finalized GtkTextBuffer.  It also stops a freshly allocated
       buffer from reusing the old address and defeating the test above. */
    if (plugin->current_buffer != NULL)
    {
        if (plugin->buffer_changed_id != 0)
        {
            g_signal_handler_disconnect (plugin->current_buffer,
                                         plugin->buffer_changed_id);
            plugin->buffer_changed_id = 0;
        }
        g_object_unref (plugin->current_buffer);
    }
    plugin->current_buffer = buffer;
    if (buffer != NULL)
    {
        g_object_ref (buffer);
        plugin->buffer_changed_id = g_signal_connect (
            buffer, "changed",
            G_CALLBACK (page_preview_on_buffer_changed), plugin);
    }
}

/* MooEditWindow "notify::active-doc" handler.  Re-wire to the new doc's
 * buffer and trigger an immediate render (no debounce — switching docs
 * isn't typing). */
static void
page_preview_on_active_doc_notify (G_GNUC_UNUSED GObject *window,
                                G_GNUC_UNUSED GParamSpec *pspec,
                                gpointer data)
{
    PagePreviewWindowPlugin *plugin = (PagePreviewWindowPlugin *) data;
    page_preview_rewire_buffer_signal (plugin);
    page_preview_render (plugin, NULL);
}

static gboolean
page_preview_window_plugin_create (PagePreviewWindowPlugin *plugin)
{
    MooEditWindow *window = MOO_WIN_PLUGIN (plugin)->window;
    GtkWidget     *scroll;
    GtkWidget     *html;
    MooPaneLabel  *label;

    /* MooHtml is a GtkTextView subclass that renders simplified HTML
     * into a GtkTextBuffer using tags — perfect for a no-WebKit preview.
     * Wrap in a GtkScrolledWindow so long documents are scrollable. */
    html   = (GtkWidget *) g_object_new (MOO_TYPE_HTML, NULL);
    gtk_text_view_set_editable      (GTK_TEXT_VIEW (html), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (html), FALSE);
    gtk_text_view_set_wrap_mode      (GTK_TEXT_VIEW (html), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request (html, 360, -1);   /* sensible default width */

    /* Bump the base font 1.2× so the preview reads at a comfortable
     * size next to the source editor.  Applied via CSS on this widget
     * only so other GtkTextViews in the window stay unaffected.  Per-
     * tag scale overrides (headings, code) compound on top of this. */
    {
        static GtkCssProvider *fp = NULL;
        if (fp == NULL)
        {
            fp = gtk_css_provider_new ();
            gtk_css_provider_load_from_data (
                fp, "textview { font-size: 120%; }", -1, NULL);
        }
        gtk_style_context_add_provider (
            gtk_widget_get_style_context (html),
            GTK_STYLE_PROVIDER (fp),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    /* Open up line spacing — MooHtml's default is tight because the tags
     * it creates per-element don't set any paragraph margins.  Adding view-
     * level defaults gives every paragraph a few pixels of breathing room
     * (overridable per-tag, which is what headings and <pre> already do). */
    /* Keep view-level pixels-above/below small so list items and
     * other line-by-line content (headings, etc.) don't get bloated
     * vertically.  Paragraph-level spacing is added in process_p_elm
     * by emitting an extra forced newline at the end of each <p>;
     * that gives one blank-line gap between paragraphs without
     * affecting <li>/<dt>/<dd> rendering. */
    gtk_text_view_set_pixels_above_lines (GTK_TEXT_VIEW (html), 2);
    gtk_text_view_set_pixels_below_lines (GTK_TEXT_VIEW (html), 2);
    gtk_text_view_set_pixels_inside_wrap (GTK_TEXT_VIEW (html), 2);
    gtk_text_view_set_left_margin   (GTK_TEXT_VIEW (html), 10);
    gtk_text_view_set_right_margin  (GTK_TEXT_VIEW (html), 10);
    gtk_text_view_set_top_margin    (GTK_TEXT_VIEW (html), 8);
    gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (html), 8);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll),
                                         GTK_SHADOW_IN);
    gtk_container_add (GTK_CONTAINER (scroll), html);
    gtk_widget_show_all (scroll);

    /* The icon-name field is the GTK stock icon shown next to the pane
     * label; "text-x-generic" is the freedesktop icon for plain text
     * which renders fine on every theme (Adwaita / Default / etc.). */
    label = moo_pane_label_new ("text-x-generic", NULL,
                                _("Page Preview"),
                                _("Page Preview"));
    plugin->pane = moo_edit_window_add_pane (window,
                                             PAGE_PREVIEW_PLUGIN_ID,
                                             scroll, label,
                                             MOO_PANE_POS_RIGHT);
    moo_pane_label_free (label);

    plugin->html_view = html;

    /* Honour the user's "show on open" preference.  When FALSE the
     * pane is still registered (so it appears in View → Panes →
     * Page Preview), it just doesn't auto-open. */
    if (moo_prefs_get_bool (PAGE_PREVIEW_SHOW_PREF))
        moo_edit_window_show_pane (window, PAGE_PREVIEW_PLUGIN_ID);

    /* Wire live-preview signals:
     *   * notify::active-doc on the window → re-target our buffer
     *     "changed" handler when the user switches tabs.
     *   * "changed" on the active doc's buffer → schedule a debounced
     *     re-render.
     * Both are torn down in page_preview_window_plugin_destroy. */
    plugin->notify_active_doc_id = g_signal_connect (
        window, "notify::active-doc",
        G_CALLBACK (page_preview_on_active_doc_notify), plugin);
    page_preview_rewire_buffer_signal (plugin);

    /* Initial render of whatever's currently the active document. */
    page_preview_render (plugin, NULL);
    return TRUE;
}

static void
page_preview_window_plugin_destroy (PagePreviewWindowPlugin *plugin)
{
    MooEditWindow *window = MOO_WIN_PLUGIN (plugin)->window;

    /* Live-preview teardown: cancel pending render, drop signal handlers. */
    if (plugin->render_timeout_id != 0)
    {
        g_source_remove (plugin->render_timeout_id);
        plugin->render_timeout_id = 0;
    }
    if (plugin->current_buffer != NULL)
    {
        if (plugin->buffer_changed_id != 0)
        {
            g_signal_handler_disconnect (plugin->current_buffer,
                                         plugin->buffer_changed_id);
            plugin->buffer_changed_id = 0;
        }
        g_object_unref (plugin->current_buffer);
        plugin->current_buffer = NULL;
    }
    if (plugin->notify_active_doc_id != 0)
    {
        g_signal_handler_disconnect (window, plugin->notify_active_doc_id);
        plugin->notify_active_doc_id = 0;
    }

    /* The pane owns the scrolled window which owns the MooHtml; removing
     * it from the paned tears the whole subtree down via GTK ref drops. */
    moo_edit_window_remove_pane (window, PAGE_PREVIEW_PLUGIN_ID);
    plugin->pane      = NULL;
    plugin->html_view = NULL;
}

static gboolean
page_preview_plugin_init (G_GNUC_UNUSED PagePreviewPlugin *plugin)
{
    moo_prefs_new_key_bool (PAGE_PREVIEW_SHOW_PREF, TRUE);
    return TRUE;
}

static void
page_preview_plugin_deinit (G_GNUC_UNUSED PagePreviewPlugin *plugin)
{
}

MOO_PLUGIN_DEFINE_INFO (page_preview,
                        "Page Preview",
                        "Live Markdown preview side pane",
                        "medit project",
                        MOO_VERSION)

MOO_WIN_PLUGIN_DEFINE (PagePreview, page_preview)

MOO_PLUGIN_DEFINE (PagePreview, page_preview,
                   NULL, NULL, NULL, NULL, NULL,
                   page_preview_window_plugin_get_type (),
                   0)

#endif /* MOO_BUILD_MARKDOWN */

extern "C" gboolean
_moo_page_preview_plugin_init (void)
{
#ifdef MOO_BUILD_MARKDOWN
    MooPluginParams params = { TRUE, TRUE };
    return moo_plugin_register (PAGE_PREVIEW_PLUGIN_ID,
                                page_preview_plugin_get_type (),
                                &page_preview_plugin_info,
                                &params);
#else
    return FALSE;
#endif
}
