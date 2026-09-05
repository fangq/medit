/*
 *   moohtml.c
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

#include "config.h"
#include "moohtml.h"
#include "marshals.h"
#include "mooutils/mooutils-misc.h"
#include "mooutils/mooutils-gpp.h"
#include "mooutils/moocompat.h"
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <mooglib/moo-glib.h>
#include <string.h>
#include <errno.h>

#include <libxml/HTMLtree.h>

#define DEFAULT_PAR_SPACING 6


struct _MooHtmlData {
    GHashTable *root_tags; /* char* -> MooHtmlTag* */
    GSList *href_tags;

    char *title;
    htmlDocPtr doc;
    GHashTable *anchors; /* char* -> GtkTextMark* */
    char *hover_link;

    double font_sizes[7];
    int par_spacing[7];
    double heading_sizes[6];
    int heading_spacing[6];
    char *heading_faces[6];
    char *monospace;
    GHashTable *font_faces;

    char *filename;
    char *basename;
    char *dirname;

    gboolean new_line;
    gboolean space;

    int list_depth;   /* current depth of <ul>/<ol> nesting (0 outside) */

    gboolean button_pressed;
    gboolean in_drag;

    GSList *rulers;
};

typedef enum {
    MOO_HTML_NONE               = 0,

    MOO_HTML_FG                 = 1 << 0,
    MOO_HTML_BG                 = 1 << 1,
    MOO_HTML_BOLD               = 1 << 2,
    MOO_HTML_ITALIC             = 1 << 3,
    MOO_HTML_UNDERLINE          = 1 << 4,
    MOO_HTML_STRIKETHROUGH      = 1 << 5,
    MOO_HTML_LINK               = 1 << 6,
    MOO_HTML_SUB                = 1 << 7,
    MOO_HTML_SUP                = 1 << 8,
    MOO_HTML_LEFT_MARGIN        = 1 << 9,

    MOO_HTML_PRE                = 1 << 10,

    MOO_HTML_MONOSPACE          = 1 << 11,
    MOO_HTML_LARGER             = 1 << 12,
    MOO_HTML_SMALLER            = 1 << 13,
    MOO_HTML_HEADING            = 1 << 14,
    MOO_HTML_FONT_SIZE          = 1 << 15,
    MOO_HTML_FONT_PT_SIZE       = 1 << 16,
    MOO_HTML_FONT_FACE          = 1 << 17,

    /* Marker flags — they don't set any text property by themselves but
     * let consumers like the Markdown preview plugin recognise the
     * element kind via the tag predicates exposed in moohtml.h. */
    MOO_HTML_BLOCKQUOTE         = 1 << 18,
    MOO_HTML_TABLE              = 1 << 19
} MooHtmlAttrMask;

MOO_DEFINE_FLAGS(MooHtmlAttrMask)

struct _MooHtmlAttr
{
    MooHtmlAttrMask mask;
    char *fg;
    char *bg;
    char *link;
    int left_margin;

    guint heading;
    guint font_size;
    guint font_pt_size;
    guint scale;
    char *font_face;
};


static MooHtmlAttr *moo_html_attr_copy      (const MooHtmlAttr *src);
static void     moo_html_attr_free          (MooHtmlAttr    *attr);

static void     moo_html_finalize           (GObject        *object);
static void     moo_html_set_property       (GObject        *object,
                                             guint           prop_id,
                                             const GValue   *value,
                                             GParamSpec     *pspec);
static void     moo_html_get_property       (GObject        *object,
                                             guint           prop_id,
                                             GValue         *value,
                                             GParamSpec     *pspec);

static MooHtmlData *moo_html_get_data       (GtkTextView    *object);

static void     moo_html_tag_finalize       (GObject        *object);
static void     moo_html_tag_set_property   (GObject        *object,
                                             guint           prop_id,
                                             const GValue   *value,
                                             GParamSpec     *pspec);
static void     moo_html_tag_get_property   (GObject        *object,
                                             guint           prop_id,
                                             GValue         *value,
                                             GParamSpec     *pspec);

static void     moo_html_size_allocate_cb   (GtkWidget      *widget,
                                             GtkAllocation  *allocation);
static void     moo_html_size_allocate      (GtkWidget      *widget,
                                             GtkAllocation  *allocation);
static gboolean moo_html_button_press       (GtkWidget      *widget,
                                             GdkEventButton *event);
static gboolean moo_html_button_release     (GtkWidget      *widget,
                                             GdkEventButton *event);
static gboolean moo_html_motion             (GtkWidget      *widget,
                                             GdkEventMotion *event);

static gboolean moo_html_load_url_real      (MooHtml        *html,
                                             const char     *url);
static void     moo_html_clear              (GtkTextView    *view);
static void     moo_html_set_doc            (GtkTextView    *view,
                                             htmlDocPtr      doc);
static void     moo_html_load_doc           (GtkTextView    *view,
                                             htmlDocPtr      doc);

static MooHtmlTag *moo_html_create_tag      (GtkTextView    *view,
                                             const MooHtmlAttr *attr,
                                             MooHtmlTag     *parent,
                                             gboolean        force);
static void     moo_html_create_anchor      (GtkTextView    *view,
                                             GtkTextBuffer  *buffer,
                                             GtkTextIter    *iter,
                                             const char     *name);
static MooHtmlTag *moo_html_get_link_tag    (GtkTextIter    *iter);
static MooHtmlTag *moo_html_get_tag         (GtkTextIter    *iter);

static gboolean moo_html_parse_url          (const char     *url,
                                             char          **scheme,
                                             char          **base,
                                             char          **anchor);
static gboolean moo_html_goto_anchor        (GtkTextView    *view,
                                             const char     *anchor);
static void     moo_html_make_heading_tag   (GtkTextView    *view,
                                             MooHtmlTag     *tag,
                                             guint           heading);

static void     init_funcs                  (void);


G_DEFINE_TYPE (MooHtml, _moo_html, GTK_TYPE_TEXT_VIEW)
G_DEFINE_TYPE (MooHtmlTag, _moo_html_tag, GTK_TYPE_TEXT_TAG)

enum {
    HTML_PROP_0,
    HTML_PROP_TITLE,
    HTML_PROP_MARKUP
};

enum {
    LOAD_URL = 0,
    HOVER_LINK,
    NUM_HTML_SIGNALS
};

enum {
    TAG_PROP_0,
    TAG_PROP_HREF
};


static guint html_signals[NUM_HTML_SIGNALS];


/************************************************************************/
/* MooHtmlTag
 */

static void
_moo_html_tag_class_init (MooHtmlTagClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->finalize = moo_html_tag_finalize;
    gobject_class->set_property = moo_html_tag_set_property;
    gobject_class->get_property = moo_html_tag_get_property;

    g_object_class_install_property (gobject_class,
                                     TAG_PROP_HREF,
                                     g_param_spec_string ("href",
                                             "href",
                                             "href",
                                             nullptr,
                                             (GParamFlags) G_PARAM_READWRITE));
}


static void
_moo_html_tag_init (MooHtmlTag *tag)
{
    tag->href = nullptr;
    tag->parent = nullptr;
    tag->attr = nullptr;
}


static void
moo_html_tag_set_property (GObject        *object,
                           guint           prop_id,
                           const GValue   *value,
                           GParamSpec     *pspec)
{
    MooHtmlTag *tag = MOO_HTML_TAG (object);

    switch (prop_id)
    {
        case TAG_PROP_HREF:
            g_free (tag->href);
            tag->href = g_strdup (g_value_get_string (value));
            g_object_notify (object, "href");
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
moo_html_tag_get_property (GObject        *object,
                           guint           prop_id,
                           GValue         *value,
                           GParamSpec     *pspec)
{
    MooHtmlTag *tag = MOO_HTML_TAG (object);

    switch (prop_id)
    {
        case TAG_PROP_HREF:
            g_value_set_string (value, tag->href);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
moo_html_tag_finalize (GObject        *object)
{
    MooHtmlTag *tag = MOO_HTML_TAG (object);

    moo_html_attr_free (tag->attr);
    g_free (tag->href);

    G_OBJECT_CLASS(_moo_html_tag_parent_class)->finalize (object);
}


/************************************************************************/
/* MooHtml
 */

static void
_moo_html_class_init (MooHtmlClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    gobject_class->finalize = moo_html_finalize;
    gobject_class->set_property = moo_html_set_property;
    gobject_class->get_property = moo_html_get_property;

    widget_class->size_allocate = moo_html_size_allocate;
    widget_class->button_press_event = moo_html_button_press;
    widget_class->button_release_event = moo_html_button_release;
    widget_class->motion_notify_event = moo_html_motion;

    klass->load_url = moo_html_load_url_real;

    init_funcs ();

    g_object_class_install_property (gobject_class,
                                     HTML_PROP_TITLE,
                                     g_param_spec_string ("title",
                                             "title",
                                             "title",
                                             nullptr,
                                             (GParamFlags) G_PARAM_READWRITE));

    g_object_class_install_property (gobject_class,
                                     HTML_PROP_MARKUP,
                                     g_param_spec_string ("markup",
                                             "markup",
                                             "markup",
                                             nullptr,
                                             G_PARAM_WRITABLE));

    html_signals[LOAD_URL] =
            g_signal_new ("load-url",
                          G_TYPE_FROM_CLASS (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooHtmlClass, load_url),
                          g_signal_accumulator_true_handled, nullptr,
                          _moo_marshal_BOOLEAN__STRING,
                          G_TYPE_BOOLEAN, 1,
                          G_TYPE_STRING);

    html_signals[HOVER_LINK] =
            g_signal_new ("hover-link",
                          G_TYPE_FROM_CLASS (klass),
                          G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                          G_STRUCT_OFFSET (MooHtmlClass, hover_link),
                          nullptr, nullptr,
                          _moo_marshal_VOID__STRING,
                          G_TYPE_NONE, 1,
                          G_TYPE_STRING);
}


static MooHtmlData *
moo_html_data_new (void)
{
    MooHtmlData *data = g_new0 (MooHtmlData, 1);

    data->anchors = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, nullptr);
    data->root_tags = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, nullptr);
    data->href_tags = nullptr;
    data->doc = nullptr;

    data->filename = nullptr;
    data->basename = nullptr;
    data->dirname = nullptr;

    data->font_sizes[0] = PANGO_SCALE_X_SMALL;
    data->font_sizes[1] = PANGO_SCALE_SMALL;
    data->font_sizes[2] = PANGO_SCALE_MEDIUM;
    data->font_sizes[3] = PANGO_SCALE_LARGE;
    data->font_sizes[4] = PANGO_SCALE_X_LARGE;
    data->font_sizes[5] = PANGO_SCALE_XX_LARGE;
    data->font_sizes[6] = PANGO_SCALE_XX_LARGE * PANGO_SCALE_LARGE;

    data->heading_sizes[0] = PANGO_SCALE_XX_LARGE * PANGO_SCALE_LARGE;
    data->heading_sizes[1] = PANGO_SCALE_XX_LARGE;
    data->heading_sizes[2] = PANGO_SCALE_X_LARGE;
    data->heading_sizes[3] = PANGO_SCALE_LARGE;
    data->heading_sizes[4] = PANGO_SCALE_MEDIUM;
    data->heading_sizes[5] = PANGO_SCALE_SMALL;

    /* Per-heading extra "pixels below" the line — added to
     * DEFAULT_PAR_SPACING (6 px) when the heading tag is built.  Used to
     * be all-zero by default, which made headings nearly flush with the
     * following paragraph.  Larger headings get more breathing room. */
    /* Less below than above — GitHub-style headings attach to their
     * section.  H1/H2 are larger so they keep a bit more breathing room. */
    data->heading_spacing[0] = 10;   /* H1 */
    data->heading_spacing[1] =  8;   /* H2 */
    data->heading_spacing[2] =  6;   /* H3 */
    data->heading_spacing[3] =  4;   /* H4 */
    data->heading_spacing[4] =  3;   /* H5 */
    data->heading_spacing[5] =  2;   /* H6 */

    data->monospace = g_strdup ("Monospace");
    data->font_faces = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    return data;
}

static void
moo_html_data_free (MooHtmlData *data)
{
    if (data)
    {
        int i;

        g_hash_table_destroy (data->anchors);
        g_hash_table_destroy (data->root_tags);
        g_slist_free (data->href_tags);
        /* moo_html_clear() frees this, but a MooHtml destroyed without a
           preceding clear (the usual case for the preview pane) did not. */
        g_slist_free (data->rulers);
        data->rulers = nullptr;

        g_free (data->title);
        g_free (data->hover_link);

        if (data->doc)
            xmlFreeDoc (data->doc);

        g_hash_table_destroy (data->font_faces);
        g_free (data->monospace);
        for (i = 0; i < 6; ++i)
            g_free (data->heading_faces[i]);

        g_free (data->filename);
        g_free (data->basename);
        g_free (data->dirname);

        g_free (data);
    }
}

static MooHtmlData *
moo_html_get_data (GtkTextView* object)
{
    MooHtmlData *data = object_get_data_cast<MooHtmlData> (object, "moo-html-data");

    if (!data)
    {
        init_funcs ();
        data = moo_html_data_new ();
        g_object_set_data_full(G_OBJECT(object), "moo-html-data", data,
                                (GDestroyNotify) moo_html_data_free);
        g_signal_connect (object, "size-allocate", G_CALLBACK (moo_html_size_allocate_cb), nullptr);
    }

    return data;
}


static void
_moo_html_init (MooHtml *html)
{
    html->data = moo_html_data_new ();
    g_object_set_data (G_OBJECT (html), "moo-html-data", html->data);
    g_object_set (html,
                  "cursor-visible", FALSE,
                  "editable", FALSE,
                  "wrap-mode", GTK_WRAP_WORD,
                  "pixels-below-lines", DEFAULT_PAR_SPACING,
                  nullptr);
}


static void
moo_html_set_title (GtkTextView *view,
                    const char  *title)
{
    MooHtmlData *data = moo_html_get_data (view);

    g_free (data->title);
    data->title = g_strdup (title);

    if (MOO_IS_HTML (view))
        g_object_notify (G_OBJECT (view), "title");
}


static void
moo_html_set_property (GObject        *object,
                       guint           prop_id,
                       const GValue   *value,
                       GParamSpec     *pspec)
{
    MooHtml *html = MOO_HTML (object);
    const char *string;

    switch (prop_id)
    {
        case HTML_PROP_TITLE:
            moo_html_set_title (GTK_TEXT_VIEW (html), g_value_get_string (value));
            break;

        case HTML_PROP_MARKUP:
            string = g_value_get_string (value);

            if (!string)
                string = "";

            _moo_html_load_memory (GTK_TEXT_VIEW (html), string, -1, nullptr, nullptr);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
moo_html_get_property (GObject        *object,
                       guint           prop_id,
                       GValue         *value,
                       GParamSpec     *pspec)
{
    MooHtml *html = MOO_HTML (object);

    switch (prop_id)
    {
        case HTML_PROP_TITLE:
            g_value_set_string (value, html->data->title);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
moo_html_finalize (GObject *object)
{
    MooHtml *html = MOO_HTML (object);
    moo_html_data_free (html->data);
    G_OBJECT_CLASS (_moo_html_parent_class)->finalize (object);
}


static gboolean
moo_html_load_url (GtkTextView *view,
                   const char  *url)
{
    char *scheme, *base, *anchor;
    gboolean result = FALSE;
    MooHtmlData *data = moo_html_get_data (view);

    g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), FALSE);
    g_return_val_if_fail (url != nullptr, FALSE);

    if (!moo_html_parse_url (url, &scheme, &base, &anchor))
    {
        g_warning ("invalid url '%s'", url);
        return FALSE;
    }

    if (!scheme)
        scheme = g_strdup ("file://");

    if (!strcmp (scheme, "mailto:") ||
        !strcmp (scheme, "mailto://"))
    {
        result = moo_open_email (base, nullptr, nullptr);
        goto out;
    }

    if (strcmp (scheme, "file://"))
        goto out;

    if (!base || (data->basename && !strcmp (data->basename, base)))
    {
        if (anchor)
            result = moo_html_goto_anchor (view, anchor);
        else
            result = TRUE;
    }
    else if (!g_path_is_absolute (base))
    {
        if (data->dirname)
        {
            char *filename = g_build_filename (data->dirname, base, nullptr);

            result = _moo_html_load_file (view, filename, nullptr);

            if (result && anchor)
                moo_html_goto_anchor (view, anchor);

            g_free (filename);
        }
    }
    else
    {
        result = _moo_html_load_file (view, base, nullptr);

        if (result && anchor)
            moo_html_goto_anchor (view, anchor);
    }

out:
    g_free (scheme);
    g_free (base);
    g_free (anchor);
    return result;
}


static gboolean
moo_html_load_url_real (MooHtml        *html,
                        const char     *url)
{
    return moo_html_load_url (GTK_TEXT_VIEW (html), url);
}


static void
remove_tag (GtkTextTag      *tag,
            GtkTextTagTable *table)
{
    gtk_text_tag_table_remove (table, tag);
}


static void
moo_html_clear (GtkTextView *view)
{
    GtkTextBuffer *buffer;
    GtkTextTagTable *table;
    GtkTextIter start, end;
    MooHtmlData *data = moo_html_get_data (view);

    g_hash_table_destroy (data->anchors);
    data->anchors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);

    buffer = gtk_text_view_get_buffer (view);
    table = gtk_text_buffer_get_tag_table (buffer);
    g_slist_foreach (data->href_tags, (GFunc)(void(*)(void)) remove_tag, table);
    g_slist_free (data->href_tags);
    data->href_tags = nullptr;

    gtk_text_buffer_get_bounds (buffer, &start, &end);
    gtk_text_buffer_delete (buffer, &start, &end);

    data->new_line = TRUE;
    data->space = TRUE;

    if (data->doc)
        xmlFreeDoc (data->doc);
    data->doc = nullptr;

    g_slist_free (data->rulers);
    data->rulers = nullptr;
}


void
moo_text_view_set_markup (GtkTextView *view,
                          const char  *markup)
{
    g_return_if_fail (GTK_IS_TEXT_VIEW (view));
    g_return_if_fail (markup != nullptr);
    _moo_html_load_memory (view, markup, -1, nullptr, nullptr);
}


gboolean
_moo_html_load_memory (GtkTextView *view,
                       const char  *buffer,
                       int          size,
                       const char  *url,
                       const char  *encoding)
{
    htmlDocPtr doc;
    MooHtmlData *data;

    g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), FALSE);
    g_return_val_if_fail (buffer != nullptr, FALSE);

    data = moo_html_get_data (view);

    if (size < 0)
        size = (int) strlen (buffer);

    doc = htmlReadMemory (buffer, size, url, encoding,
                          HTML_PARSE_NONET);

    if (!doc)
    {
        g_warning ("htmlReadMemory failed");
        return FALSE;
    }

    g_free (data->filename);
    g_free (data->basename);
    g_free (data->dirname);
    data->filename = nullptr;
    data->basename = nullptr;
    data->dirname = nullptr;

    moo_html_set_doc (view, doc);

    xmlCleanupParser ();
    return TRUE;
}


gboolean
_moo_html_load_file (GtkTextView *view,
                     const char  *file,
                     const char  *encoding)
{
    htmlDocPtr doc;
    MooHtmlData *data = moo_html_get_data (view);

    g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), FALSE);
    g_return_val_if_fail (file != nullptr, FALSE);

    doc = htmlReadFile (file, encoding, HTML_PARSE_NONET);

    if (!doc)
        return FALSE;

    g_free (data->filename);
    g_free (data->basename);
    g_free (data->dirname);

    data->filename = g_strdup (file);
    data->basename = g_path_get_basename (file);
    data->dirname = g_path_get_dirname (file);

    moo_html_set_doc (view, doc);

    xmlCleanupParser ();
    return TRUE;
}


static void
moo_html_set_doc (GtkTextView *view,
                  htmlDocPtr   doc)
{
    MooHtmlData *data;

    g_return_if_fail (doc != nullptr);

    data = moo_html_get_data (view);
    g_return_if_fail (doc != data->doc);

    moo_html_clear (view);

    data->doc = doc;
    moo_html_load_doc (view, doc);
}


static void
attr_compose (MooHtmlAttr       *dest,
              const MooHtmlAttr *src)
{
    static MooHtmlAttrMask simple =
            MOO_HTML_BOLD | MOO_HTML_ITALIC | MOO_HTML_UNDERLINE |
            MOO_HTML_STRIKETHROUGH | MOO_HTML_MONOSPACE | MOO_HTML_SUB |
            MOO_HTML_SUP | MOO_HTML_PRE |
            MOO_HTML_BLOCKQUOTE | MOO_HTML_TABLE;
    static MooHtmlAttrMask font_size_mask =
            MOO_HTML_LARGER | MOO_HTML_SMALLER | MOO_HTML_HEADING |
            MOO_HTML_FONT_SIZE | MOO_HTML_FONT_PT_SIZE;
    static MooHtmlAttrMask font_face_mask =
            MOO_HTML_FONT_FACE | MOO_HTML_MONOSPACE;

    g_return_if_fail (dest != nullptr);

    if (!src)
        return;

    dest->mask |= (src->mask & simple);

    if (src->mask & MOO_HTML_FG)
    {
        dest->mask |= MOO_HTML_FG;
        dest->fg = src->fg;
    }

    if (src->mask & MOO_HTML_BG)
    {
        dest->mask |= MOO_HTML_BG;
        dest->bg = src->bg;
    }

    if (src->mask & MOO_HTML_LINK)
    {
        dest->mask |= MOO_HTML_LINK;
        dest->link = src->link;
    }

    if (src->mask & MOO_HTML_LEFT_MARGIN)
    {
        if (!(dest->mask & MOO_HTML_LEFT_MARGIN))
        {
            dest->mask |= MOO_HTML_LEFT_MARGIN;
            dest->left_margin = src->left_margin;
        }
        else
        {
            dest->left_margin += src->left_margin;
        }
    }

    if ((src->mask & font_face_mask) && !(dest->mask & font_face_mask))
    {
        dest->mask |= (src->mask & font_face_mask);
        dest->font_face = src->font_face;
    }

    if (dest->mask & MOO_HTML_HEADING)
    {
        g_assert (1 <= dest->heading && dest->heading <= 6);
    }
    else if (src->mask & MOO_HTML_HEADING)
    {
        dest->mask &= ~font_size_mask;
        dest->mask |= MOO_HTML_HEADING;
        dest->heading = src->heading;
        g_assert (1 <= dest->heading && dest->heading <= 6);
    }
    else if (dest->mask & (MOO_HTML_LARGER | MOO_HTML_SMALLER))
    {
        int size = 3;
        int scale = (dest->mask & MOO_HTML_LARGER) ? (int)dest->scale : -((int)dest->scale);

        if (src->mask & (MOO_HTML_LARGER | MOO_HTML_SMALLER))
        {
            if (src->mask & MOO_HTML_LARGER)
                scale += src->scale;
            else
                scale -= src->scale;
        }
        else if (src->mask & MOO_HTML_FONT_SIZE)
        {
            size = src->font_size;
        }
        else if (src->mask & MOO_HTML_FONT_PT_SIZE)
        {
            /* XXX ??? */
        }

        size += scale;
        size = CLAMP (size, 1, 7);

        dest->mask &= ~font_size_mask;
        dest->mask |= MOO_HTML_FONT_SIZE;
        dest->font_size = size;
    }
    else if (dest->mask & MOO_HTML_FONT_SIZE)
    {
        dest->mask &= ~font_size_mask;
        dest->mask |= MOO_HTML_FONT_SIZE;
    }
    else if (dest->mask & MOO_HTML_FONT_PT_SIZE)
    {
        dest->mask &= ~font_size_mask;
        dest->mask |= MOO_HTML_FONT_PT_SIZE;
    }
    else
    {
        dest->mask &= ~font_size_mask;
        dest->mask |= (src->mask & font_size_mask);
        dest->heading = src->heading;
        dest->font_size = src->font_size;
        dest->font_pt_size = src->font_pt_size;
        dest->scale = src->scale;
    }
}


static void
attr_apply (const MooHtmlAttr *attr,
            MooHtmlTag        *tag,
            GtkTextView       *view)
{
    MooHtmlData *data = moo_html_get_data (view);

    g_return_if_fail (attr != nullptr && tag != nullptr);

    moo_html_attr_free (tag->attr);
    tag->attr = moo_html_attr_copy (attr);

    if (attr->mask & MOO_HTML_FG)
        g_object_set (tag, "foreground", attr->fg, nullptr);
    if (attr->mask & MOO_HTML_BG)
        g_object_set (tag, "background", attr->bg, nullptr);
    if (attr->mask & MOO_HTML_BOLD)
        g_object_set (tag, "weight", PANGO_WEIGHT_BOLD, nullptr);
    if (attr->mask & MOO_HTML_ITALIC)
        g_object_set (tag, "style", PANGO_STYLE_ITALIC, nullptr);
    if (attr->mask & MOO_HTML_UNDERLINE)
        g_object_set (tag, "underline", PANGO_UNDERLINE_SINGLE, nullptr);
    if (attr->mask & MOO_HTML_STRIKETHROUGH)
        g_object_set (tag, "strikethrough", TRUE, nullptr);

    if (attr->mask & MOO_HTML_LEFT_MARGIN)
        g_object_set (tag, "left-margin", attr->left_margin, nullptr);

    if (attr->mask & MOO_HTML_LINK)
    {
        g_free (tag->href);
        tag->href = g_strdup (attr->link);
        g_object_set (tag, "foreground", "blue", nullptr);
    }

    if (attr->mask & MOO_HTML_SUP)
        g_object_set (tag,
                      "rise", 8 * PANGO_SCALE,
                      "size", 8 * PANGO_SCALE,
                      nullptr);
    if (attr->mask & MOO_HTML_SUB)
        g_object_set (tag,
                      "rise", -8 * PANGO_SCALE,
                      "size", 8 * PANGO_SCALE,
                      nullptr);

    if (attr->mask & MOO_HTML_MONOSPACE)
        g_object_set (tag, "font", data->monospace, nullptr);
    else if (attr->mask & MOO_HTML_FONT_FACE)
        g_object_set (tag, "font", attr->font_face, nullptr);

    if (attr->mask & MOO_HTML_HEADING)
    {
        moo_html_make_heading_tag (view, tag, attr->heading);
    }
    else if (attr->mask & MOO_HTML_LARGER)
    {
        double scale;
        int space;
        int size = 3 + attr->scale;
        size = CLAMP (size, 1, 7);
        scale = data->font_sizes[size - 1];
        space = data->par_spacing[size - 1];
        g_object_set (tag,
                      "scale", scale,
                      "pixels-below-lines", DEFAULT_PAR_SPACING + space,
                      nullptr);
    }
    else if (attr->mask & MOO_HTML_SMALLER)
    {
        double scale;
        int space;
        int size = 3 - attr->scale;
        size = CLAMP (size, 1, 7);
        scale = data->font_sizes[size - 1];
        space = data->par_spacing[size - 1];
        g_object_set (tag,
                      "scale", scale,
                      "pixels-below-lines", DEFAULT_PAR_SPACING + space,
                      nullptr);
    }
    else if (attr->mask & MOO_HTML_FONT_SIZE)
    {
        g_assert (1 <= attr->font_size && attr->font_size <= 7);
        g_object_set (tag,
                      "scale", data->font_sizes[attr->font_size - 1],
                      "pixels-below-lines",
                      DEFAULT_PAR_SPACING + data->par_spacing[attr->font_size - 1],
                      nullptr);
    }
    else if (attr->mask & MOO_HTML_FONT_PT_SIZE)
    {
        g_object_set (tag, "size-points", (double) attr->font_pt_size, nullptr);
    }
}


static void
moo_html_make_heading_tag (GtkTextView    *view,
                           MooHtmlTag     *tag,
                           guint           heading)
{
    MooHtmlData *data = moo_html_get_data (view);

    g_assert (1 <= heading && heading <= 6);

    /* Also reserve space ABOVE the heading so it doesn't crowd the
     * preceding paragraph.  Slightly less than the below-spacing so
     * the heading visually attaches to the section it introduces. */
    /* Above-spacing is bigger than below-spacing so the heading sits
     * closer to its own section than to the preceding paragraph. */
    g_object_set (tag,
                  "pixels-above-lines",
                  DEFAULT_PAR_SPACING + data->heading_spacing[heading - 1],
                  "pixels-below-lines",
                  data->heading_spacing[heading - 1] / 2,
                  "scale", data->heading_sizes[heading - 1],
                  "weight", PANGO_WEIGHT_BOLD, nullptr);

    if (data->heading_faces[heading - 1])
        g_object_set (tag, "family",
                      data->heading_faces[heading - 1], nullptr);
}


static MooHtmlTag*
moo_html_create_tag (GtkTextView       *view,
                     const MooHtmlAttr *attr,
                     MooHtmlTag        *parent,
                     gboolean           force)
{
    MooHtmlTag *tag;
    MooHtmlAttr real_attr;
    MooHtmlData *data = moo_html_get_data (view);

    g_return_val_if_fail (attr != nullptr, nullptr);

    if (!attr->mask && !force)
        return parent;

    if (parent && parent->attr)
    {
        real_attr = *parent->attr;
        attr_compose (&real_attr, attr);
    }
    else
    {
        real_attr = *attr;
    }

    tag = MOO_HTML_TAG (g_object_new (MOO_TYPE_HTML_TAG, (const char*) nullptr));
    gtk_text_tag_table_add (gtk_text_buffer_get_tag_table (gtk_text_view_get_buffer (view)),
                            GTK_TEXT_TAG (tag));
    g_object_unref (tag);

    if (tag->href)
        data->href_tags = g_slist_prepend (data->href_tags, tag);

    attr_apply (&real_attr, tag, view);

    return tag;
}


static MooHtmlAttr*
moo_html_attr_copy (const MooHtmlAttr *src)
{
    MooHtmlAttr *attr;

    g_return_val_if_fail (src != nullptr, nullptr);

    attr = g_new (MooHtmlAttr, 1);

    *attr = *src;
    attr->fg = g_strdup (src->fg);
    attr->bg = g_strdup (src->bg);
    attr->link = g_strdup (src->link);
    attr->font_face = g_strdup (src->font_face);

    return attr;
}


static void
moo_html_attr_free (MooHtmlAttr        *attr)
{
    if (attr)
    {
        g_free (attr->fg);
        g_free (attr->bg);
        g_free (attr->link);
        g_free (attr->font_face);
        g_free (attr);
    }
}


static void
moo_html_create_anchor (GtkTextView    *view,
                        GtkTextBuffer  *buffer,
                        GtkTextIter    *iter,
                        const char     *name)
{
    GtkTextMark *mark;
    char *alt_name;
    MooHtmlData *data = moo_html_get_data (view);

    g_return_if_fail (name != nullptr && (name[0] != '#' || name[1]));

    mark = gtk_text_buffer_create_mark (buffer, nullptr, iter, TRUE);

    if (name[0] == '#')
        alt_name = g_strdup (name + 1);
    else
        alt_name = g_strdup_printf ("#%s", name);

    g_hash_table_insert (data->anchors, g_strdup (name), mark);
    g_hash_table_insert (data->anchors, alt_name, mark);
}


static gboolean
moo_html_motion (GtkWidget      *widget,
                 GdkEventMotion *event)
{
    GtkTextView *textview = GTK_TEXT_VIEW (widget);
    MooHtml *html = MOO_HTML (widget);
    GtkTextIter iter;
    int buf_x, buf_y, x, y, dummy;
    GdkModifierType state;
    MooHtmlTag *tag;

    if (html->data->button_pressed)
        html->data->in_drag = TRUE;

    if (event->window != gtk_text_view_get_window (textview, GTK_TEXT_WINDOW_TEXT))
        goto out;

    if (event->is_hint)
    {
        gdk_window_get_pointer (event->window, &x, &y, &state);
    }
    else
    {
        x = event->x;
        y = event->y;
        state = GdkModifierType (event->state);
    }

    if (state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK))
        goto out;

    gtk_text_view_window_to_buffer_coords (textview, GTK_TEXT_WINDOW_TEXT,
                                           x, y, &buf_x, &buf_y);
    gtk_text_view_get_iter_at_position (textview, &iter, &dummy, buf_x, buf_y);

    tag = moo_html_get_link_tag (&iter);

    if (tag)
    {
        g_return_val_if_fail (tag->href != nullptr, FALSE);

        if (!html->data->hover_link || strcmp (html->data->hover_link, tag->href))
        {
            GdkCursor *cursor;

            g_free (html->data->hover_link);
            html->data->hover_link = g_strdup (tag->href);

            cursor = gdk_cursor_new_for_display (gdk_display_get_default (), GDK_HAND2);
            gdk_window_set_cursor (event->window, cursor);
            g_object_unref (cursor);

            g_signal_emit (html, html_signals[HOVER_LINK], 0, tag->href);
        }
    }
    else if (html->data->hover_link)
    {
        GdkCursor *cursor;

        g_free (html->data->hover_link);
        html->data->hover_link = nullptr;

        cursor = gdk_cursor_new_for_display (gdk_display_get_default (), GDK_XTERM);
        gdk_window_set_cursor (event->window, cursor);
        g_object_unref (cursor);

        g_signal_emit (html, html_signals[HOVER_LINK], 0, nullptr);
    }

out:
    return GTK_WIDGET_CLASS(_moo_html_parent_class)->motion_notify_event (widget, event);
}


static MooHtmlTag*
moo_html_get_link_tag (GtkTextIter *iter)
{
    MooHtmlTag *tag = moo_html_get_tag (iter);
    return (tag && tag->href) ? tag : nullptr;
}


static MooHtmlTag*
moo_html_get_tag (GtkTextIter *iter)
{
    MooHtmlTag *tag = nullptr;
    GSList *l;
    GSList *list = gtk_text_iter_get_tags (iter);

    for (l = list; l != nullptr; l = l->next)
    {
        if (MOO_IS_HTML_TAG (l->data))
        {
            tag = MOO_HTML_TAG (l->data);
            break;
        }
    }

    g_slist_free (list);
    return tag;
}


static gboolean
moo_html_button_press (GtkWidget      *widget,
                       GdkEventButton *event)
{
    MooHtml *html = MOO_HTML (widget);
    html->data->button_pressed = TRUE;
    html->data->in_drag = FALSE;
    return GTK_WIDGET_CLASS(_moo_html_parent_class)->button_press_event (widget, event);
}


static gboolean
moo_html_button_release (GtkWidget      *widget,
                         GdkEventButton *event)
{
    GtkTextView *textview = GTK_TEXT_VIEW (widget);
    MooHtml *html = MOO_HTML (widget);
    GtkTextIter iter;
    int buf_x, buf_y, dummy;
    MooHtmlTag *tag;

    html->data->button_pressed = FALSE;

    if (html->data->in_drag)
    {
        html->data->in_drag = FALSE;
        goto out;
    }

    if (event->window != gtk_text_view_get_window (textview, GTK_TEXT_WINDOW_TEXT))
        goto out;

    gtk_text_view_window_to_buffer_coords (textview, GTK_TEXT_WINDOW_TEXT,
                                           event->x, event->y, &buf_x, &buf_y);
    gtk_text_view_get_iter_at_position (textview, &iter, &dummy, buf_x, buf_y);

    tag = moo_html_get_link_tag (&iter);

    if (tag)
    {
        gboolean result;
        g_assert (tag->href != nullptr);
        g_signal_emit (html, html_signals[LOAD_URL], 0, tag->href, &result);
    }

out:
    return GTK_WIDGET_CLASS(_moo_html_parent_class)->button_release_event (widget, event);
}


static gboolean
moo_html_parse_url (const char     *url,
                    char          **scheme,
                    char          **base,
                    char          **anchor)
{
    GRegex *regex;
    GMatchInfo *match_info;

    g_return_val_if_fail (url != nullptr, FALSE);
    g_return_val_if_fail (scheme && base && anchor, FALSE);

    regex = g_regex_new ("^([a-zA-Z]+:(//)?)?([^#]*)(#(.*))?$", GRegexCompileFlags (0), GRegexMatchFlags (0), nullptr);
    g_return_val_if_fail (regex != nullptr, FALSE);

    if (!g_regex_match (regex, url, GRegexMatchFlags (0), &match_info))
    {
        g_match_info_free (match_info);
        g_regex_unref (regex);
        return FALSE;
    }

    *scheme = g_match_info_fetch (match_info, 1);
    *base = g_match_info_fetch (match_info, 3);
    *anchor = g_match_info_fetch (match_info, 5);

    if (!*scheme || !**scheme) {g_free (*scheme); *scheme = nullptr;}
    if (!*base || !**base) {g_free (*base); *base = nullptr;}
    if (!*anchor || !**anchor) {g_free (*anchor); *anchor = nullptr;}

    g_match_info_free (match_info);
    g_regex_unref (regex);
    return TRUE;
}


static gboolean
moo_html_goto_anchor (GtkTextView *view,
                      const char  *anchor)
{
    GtkTextMark *mark;
    MooHtmlData *data = moo_html_get_data (view);

    g_return_val_if_fail (anchor != nullptr, FALSE);

    mark = (GtkTextMark*) g_hash_table_lookup (data->anchors, anchor);

    if (!mark)
    {
        g_warning ("could not find anchor '%s'",
                   anchor);
        return FALSE;
    }
    else
    {
        gtk_text_view_scroll_to_mark (view, mark, 0.1, TRUE, 0, 0);
        return TRUE;
    }
}


#if 0
void
moo_html_set_font (MooHtml            *html,
                   const char         *string)
{
    PangoFontDescription *font;

    g_return_if_fail (MOO_IS_HTML (html));
    g_return_if_fail (string != nullptr);

    font = pango_font_description_from_string (string);
    g_return_if_fail (font != nullptr);

    gtk_widget_override_font (GTK_WIDGET (html), font);
    pango_font_description_free (font);
}
#endif


static void
moo_html_size_allocate_real (GtkWidget *widget,
                             G_GNUC_UNUSED GtkAllocation *allocation)
{
    int border_width, child_width;
    GSList *l;
    GdkWindow *window;
    MooHtmlData *data = moo_html_get_data (GTK_TEXT_VIEW (widget));

    if (!gtk_widget_get_realized (widget))
        return;

    window = gtk_text_view_get_window (GTK_TEXT_VIEW (widget), GTK_TEXT_WINDOW_TEXT);
    g_return_if_fail (window != nullptr);

    child_width = gdk_window_get_width (window);
    border_width = gtk_container_get_border_width (GTK_CONTAINER (widget));
    child_width -= 2 * border_width + 2 * 1 /* GTK3: use CSS padding instead */ +
            gtk_text_view_get_left_margin (GTK_TEXT_VIEW (widget)) +
            gtk_text_view_get_right_margin (GTK_TEXT_VIEW (widget));
    child_width = MAX (child_width, 0);

    for (l = data->rulers; l != nullptr; l = l->next)
    {
        GtkWidget *ruler = GTK_WIDGET (l->data);
        gtk_widget_set_size_request (ruler, child_width, -1);
    }
}

static void
moo_html_size_allocate (GtkWidget      *widget,
                        GtkAllocation  *allocation)
{
    GTK_WIDGET_CLASS(_moo_html_parent_class)->size_allocate (widget, allocation);
    moo_html_size_allocate_real (widget, allocation);
}

static void
moo_html_size_allocate_cb (GtkWidget     *widget,
                           GtkAllocation *allocation)
{
    moo_html_size_allocate_real (widget, allocation);
}


/***********************************************************************/
/* Loading into text buffer
 */

#define IS_ELEMENT(node_)           (node_ && node_->type == XML_ELEMENT_NODE)
#define NODE_NAME_IS_(node_,name_)  (node_->name && !strcmp ((char*)node_->name, name_))
#define IS_NAMED_ELM_(node_,name_)  (IS_ELEMENT(node_) && NODE_NAME_IS_(node_, name_))

#define IS_HEAD_ELEMENT(node_)      IS_NAMED_ELM_(node_, "head")
#define IS_BODY_ELEMENT(node_)      IS_NAMED_ELM_(node_, "body")
#define IS_TITLE_ELEMENT(node_)     IS_NAMED_ELM_(node_, "title")
#define IS_META_ELEMENT(node_)      IS_NAMED_ELM_(node_, "meta")
#define IS_LINK_ELEMENT(node_)      IS_NAMED_ELM_(node_, "link")
#define IS_LI_ELEMENT(node_)        IS_NAMED_ELM_(node_, "li")
#define IS_IMG_ELEMENT(node_)       IS_NAMED_ELM_(node_, "img")

#define IS_HEADING_ELEMENT(node_)   (IS_ELEMENT(node_) && node_->name &&                    \
                                     node_->name[0] == 'h' &&                               \
                                     ('1' <= node_->name[1] && node_->name[1] <= '6') &&    \
                                     !node_->name[2])

#define IS_TEXT(node_)              (node_ && node_->type == XML_TEXT_NODE)
#define IS_COMMENT(node_)           (node_ && node_->type == XML_COMMENT_NODE)

#define STR_FREE(s__) if (s__) xmlFree (s__)


static void moo_html_load_head      (GtkTextView    *view,
                                     xmlNode        *node);
static void moo_html_load_body      (GtkTextView    *view,
                                     xmlNode        *node);
static void moo_html_new_line       (GtkTextView    *view,
                                     GtkTextBuffer  *buffer,
                                     GtkTextIter    *iter,
                                     MooHtmlTag     *tag,
                                     gboolean        force);
static void moo_html_insert_text    (GtkTextView    *view,
                                     GtkTextBuffer  *buffer,
                                     GtkTextIter    *iter,
                                     MooHtmlTag     *tag,
                                     const char     *text);
static void moo_html_insert_verbatim(GtkTextView    *view,
                                     GtkTextBuffer  *buffer,
                                     GtkTextIter    *iter,
                                     MooHtmlTag     *tag,
                                     const char     *text);


static void
moo_html_load_doc (GtkTextView *view,
                   htmlDocPtr   doc)
{
    xmlNode *root, *node;

    root = xmlDocGetRootElement (doc);

    if (!root)
    {
        g_message ("moo_html_load_doc: empty document");
        return;
    }

    for (node = root->children; node != nullptr; node = node->next)
    {
        if (IS_HEAD_ELEMENT (node))
        {
            moo_html_load_head (view, node);
        }
        else if (IS_BODY_ELEMENT (node))
        {
            moo_html_load_body (view, node);
        }
        else
        {
            g_warning ("unknown node '%s'", node->name);
        }
    }
}


static void
moo_html_load_head (GtkTextView *view,
                    xmlNode     *node)
{
    xmlNode *child;

    for (child = node->children; child != nullptr; child = child->next)
    {
        if (IS_TITLE_ELEMENT (child))
        {
            xmlChar *title = xmlNodeGetContent (child);
            moo_html_set_title (view, (char *) title);
            STR_FREE (title);
        }
        else if (IS_META_ELEMENT (child))
        {
        }
        else if (IS_LINK_ELEMENT (child))
        {
        }
        else
        {
            g_message ("unknown node '%s'", child->name);
        }
    }
}


static void
moo_html_new_line (GtkTextView   *view,
                   GtkTextBuffer *buffer,
                   GtkTextIter   *iter,
                   MooHtmlTag    *tag,
                   gboolean       force)
{
    MooHtmlData *data = moo_html_get_data (view);

    if (!data->new_line || force)
    {
        if (tag)
            gtk_text_buffer_insert_with_tags (buffer, iter, "\n", 1,
                                              GTK_TEXT_TAG (tag), nullptr);
        else
            gtk_text_buffer_insert (buffer, iter, "\n", 1);
    }

    data->new_line = TRUE;
    data->space = TRUE;
}


static const char*
str_find_separator (const char *str)
{
    const char *p;

    for (p = str; *p; ++p)
    {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
            return p;
    }

    return nullptr;
}


static void
moo_html_insert_text (GtkTextView   *view,
                      GtkTextBuffer *buffer,
                      GtkTextIter   *iter,
                      MooHtmlTag    *tag,
                      const char    *text)
{
    const char *p;
    MooHtmlData *data = moo_html_get_data (view);

    if (tag && tag->attr && (tag->attr->mask & MOO_HTML_PRE))
    {
        moo_html_insert_verbatim (view, buffer, iter, tag, text);
        return;
    }

    while (*text)
    {
        p = str_find_separator (text);

        if (p)
        {
            if (p != text)
            {
                if (tag)
                {
                    gtk_text_buffer_insert_with_tags (buffer, iter,
                                                      text, p - text,
                                                      GTK_TEXT_TAG (tag), nullptr);
                    gtk_text_buffer_insert_with_tags (buffer, iter, " ", 1,
                                                      GTK_TEXT_TAG (tag), nullptr);
                }
                else
                {
                    gtk_text_buffer_insert (buffer, iter, text, p - text);
                    gtk_text_buffer_insert (buffer, iter, " ", 1);
                }

                data->space = TRUE;
                data->new_line = FALSE;
                text = ++p;
            }
            else
            {
                if (!data->space)
                {
                    gtk_text_buffer_insert (buffer, iter, " ", 1);
                    data->space = TRUE;
                }

                text++;
            }
        }
        else
        {
            if (tag)
                gtk_text_buffer_insert_with_tags (buffer, iter,
                                                  text, -1,
                                                  GTK_TEXT_TAG (tag), nullptr);
            else
                gtk_text_buffer_insert (buffer, iter, text, -1);
            data->new_line = FALSE;
            data->space = FALSE;
            break;
        }
    }
}


static gboolean
str_has_trailing_nl (const char *text, int len)
{
    g_assert (len > 0);

    for (len = len - 1; len >= 0; --len)
    {
        if (text[len] == '\n' || text[len] == '\r')
            return TRUE;
        else if (text[len] != ' ' && text[len] != '\t')
            return FALSE;
    }

    return FALSE;
}


static gboolean
str_has_trailing_space (const char *text, int len)
{
    g_assert (len > 0);

    if (text[len-1] == '\n' || text[len-1] == '\r' || text[len-1] == ' ' || text[len-1] == '\t')
        return TRUE;
    else
        return FALSE;
}


static void
moo_html_insert_verbatim (GtkTextView   *view,
                          GtkTextBuffer *buffer,
                          GtkTextIter   *iter,
                          MooHtmlTag    *tag,
                          const char    *text)
{
    guint len;
    MooHtmlData *data = moo_html_get_data (view);

    g_return_if_fail (text != nullptr);

    if (text[0] == '\n' && data->new_line)
        text++;

    len = (guint) strlen (text);

    if (!len)
        return;

    if (tag)
        gtk_text_buffer_insert_with_tags (buffer, iter,
                                          text, len,
                                          GTK_TEXT_TAG (tag), nullptr);
    else
        gtk_text_buffer_insert (buffer, iter, text, len);

    data->new_line = str_has_trailing_nl (text, len);

    if (data->new_line)
        data->space = TRUE;
    else
        data->space = str_has_trailing_space (text, len);
}


static void process_elm_body    (GtkTextView    *view,
                                 GtkTextBuffer  *buffer,
                                 xmlNode        *elm,
                                 MooHtmlTag     *current,
                                 GtkTextIter    *iter);
static void process_text_node   (GtkTextView    *view,
                                 GtkTextBuffer  *buffer,
                                 xmlNode        *node,
                                 MooHtmlTag     *current,
                                 GtkTextIter    *iter);
static void process_heading_elm (GtkTextView    *view,
                                 GtkTextBuffer  *buffer,
                                 xmlNode        *elm,
                                 MooHtmlTag     *current,
                                 GtkTextIter    *iter);
static void process_format_elm  (GtkTextView    *view,
                                 MooHtmlAttr    *attr,
                                 GtkTextBuffer  *buffer,
                                 xmlNode        *elm,
                                 MooHtmlTag     *current,
                                 GtkTextIter    *iter);

static void process_img_elm     (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *current, GtkTextIter *iter);
static void process_p_elm       (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_a_elm       (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_pre_elm     (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_ol_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_ul_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_font_elm    (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_cite_elm    (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_li_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_dt_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_dl_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_dd_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_br_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_div_elm     (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_span_elm    (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_hr_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);

static void process_table_elm   (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static void process_tr_elm      (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                                 MooHtmlTag *parent, GtkTextIter *iter);
static GtkCssProvider *table_css_provider (void);
static void            xml_cell_to_pango   (xmlNode *elm, GString *out);


static void
moo_html_load_body (GtkTextView *view,
                    xmlNode     *node)
{
    GtkTextIter iter;
    GtkTextBuffer *buffer;
    MooHtmlData *data = moo_html_get_data (view);

    data->new_line = TRUE;
    data->space = TRUE;

    buffer = gtk_text_view_get_buffer (view);
    gtk_text_buffer_get_end_iter (buffer, &iter);

    process_elm_body (view, buffer, node, nullptr, &iter);
}


typedef struct {
    MooHtmlAttrMask mask;
    const char *name;
} MaskNamePair;

static MooHtmlAttr*
get_format_elm_attr (xmlNode *node)
{
    static GHashTable *elms = nullptr;
    MooHtmlAttr *attr;

    if (!IS_ELEMENT (node))
        return nullptr;

    if (!elms)
    {
        guint i;
        static MaskNamePair attrs[] = {
            { MOO_HTML_BOLD, "strong" },
            { MOO_HTML_BOLD, "b" },
            { MOO_HTML_ITALIC, "em" },
            { MOO_HTML_ITALIC, "i" },
            { MOO_HTML_ITALIC, "address" },
            { MOO_HTML_UNDERLINE, "ins" },
            { MOO_HTML_UNDERLINE, "u" },
            { MOO_HTML_STRIKETHROUGH, "del" },
            { MOO_HTML_STRIKETHROUGH, "s" },
            { MOO_HTML_STRIKETHROUGH, "strike" },
            { MOO_HTML_MONOSPACE, "code" },
            { MOO_HTML_MONOSPACE, "dfn" },
            { MOO_HTML_MONOSPACE, "samp" },
            { MOO_HTML_MONOSPACE, "kbd" },
            { MOO_HTML_MONOSPACE, "var" },
            { MOO_HTML_MONOSPACE, "tt" },
            { MOO_HTML_SUB, "sub" },
            { MOO_HTML_SUP, "sup" }
        };

        elms = g_hash_table_new (g_str_hash, g_str_equal);

        for (i = 0; i < G_N_ELEMENTS (attrs); ++i)
        {
            attr = g_new0 (MooHtmlAttr, 1);
            attr->mask = attrs[i].mask;
            g_hash_table_insert (elms, (char*) attrs[i].name, attr);
        }

        attr = g_new0 (MooHtmlAttr, 1);
        attr->mask = MOO_HTML_LARGER;
        attr->scale = 1;
        g_hash_table_insert (elms, (char*) "big", attr);

        attr = g_new0 (MooHtmlAttr, 1);
        attr->mask = MOO_HTML_SMALLER;
        attr->scale = 1;
        g_hash_table_insert (elms, (char*) "small", attr);
    }

    return (MooHtmlAttr*) g_hash_table_lookup (elms, node->name);
}


typedef void (*ProcessElm) (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                            MooHtmlTag *parent, GtkTextIter *iter);

static GHashTable *proc_elm_funcs__ = nullptr;

static void
add_func__ (const char *static_elm_name,
            ProcessElm  func)
{
    g_hash_table_insert (proc_elm_funcs__, (char*) static_elm_name, (void*) func);
}

static void
init_funcs (void)
{
    if (!proc_elm_funcs__)
    {
        proc_elm_funcs__ = g_hash_table_new (g_str_hash, g_str_equal);

        add_func__ ("p", process_p_elm);
        add_func__ ("a", process_a_elm);
        add_func__ ("ol", process_ol_elm);
        add_func__ ("ul", process_ul_elm);
        add_func__ ("font", process_font_elm);
        add_func__ ("pre", process_pre_elm);
        add_func__ ("cite", process_cite_elm);
        add_func__ ("ul", process_ul_elm);
        add_func__ ("li", process_li_elm);
        add_func__ ("hr", process_hr_elm);
        add_func__ ("img", process_img_elm);
        add_func__ ("dt", process_dt_elm);
        add_func__ ("dl", process_dl_elm);
        add_func__ ("dd", process_dd_elm);
        add_func__ ("br", process_br_elm);
        add_func__ ("div", process_div_elm);
        add_func__ ("span", process_span_elm);
    }
}

static ProcessElm
get_proc_elm_func (xmlNode *node)
{
    if (IS_ELEMENT (node))
        return (ProcessElm) g_hash_table_lookup (proc_elm_funcs__, node->name);
    else
        return nullptr;
}


static void
process_elm_body (GtkTextView    *view,
                  GtkTextBuffer  *buffer,
                  xmlNode        *elm,
                  MooHtmlTag     *current,
                  GtkTextIter    *iter)
{
    xmlNode *child;
    MooHtmlAttr *attr;
    ProcessElm func;

    for (child = elm->children; child != nullptr; child = child->next)
    {

        if (IS_TEXT (child))
            process_text_node (view, buffer, child, current, iter);
        else if (IS_HEADING_ELEMENT (child))
            process_heading_elm (view, buffer, child, current, iter);
        else if ((func = get_proc_elm_func (child)))
            func (view, buffer, child, current, iter);
        else if ((attr = get_format_elm_attr (child)))
            process_format_elm (view, attr, buffer, child, current, iter);

        else if (IS_NAMED_ELM_ (child, "table"))
            process_table_elm (view, buffer, child, current, iter);
        else if (IS_NAMED_ELM_ (child, "tr"))
            process_tr_elm (view, buffer, child, current, iter);

        else if (IS_NAMED_ELM_ (child, "td") ||
                 IS_NAMED_ELM_ (child, "th") ||
                 IS_NAMED_ELM_ (child, "thead") ||
                 IS_NAMED_ELM_ (child, "tbody") ||
                 IS_NAMED_ELM_ (child, "tfoot") ||
                 IS_NAMED_ELM_ (child, "col") ||
                 IS_NAMED_ELM_ (child, "colgroup"))
        {
            process_elm_body (view, buffer, child, current, iter);
        }
        else if (IS_NAMED_ELM_ (child, "blockquote"))
        {
            /* Render <blockquote> as a widget anchor: a single GtkLabel
             * with Pango markup, wrapped in a CSS-styled container that
             * draws a continuous left border across all wrapped lines.
             * The "▎" character trick only painted the bar on the first
             * line of each paragraph — a widget with border-left gets
             * it right.  Rich content inside blockquotes (links, code)
             * survives via the same Pango-markup pass used for table
             * cells. */
            GString            *m   = g_string_new (NULL);
            GtkWidget          *box = gtk_event_box_new ();
            GtkWidget          *lbl = gtk_label_new (NULL);
            GtkTextChildAnchor *bq_anchor;
            MooHtmlData        *bq_data = moo_html_get_data (view);

            xml_cell_to_pango (child, m);
            /* Trim any leading/trailing whitespace from the markup so
             * the widget doesn't show stray blank lines. */
            g_strstrip (m->str);

            gtk_label_set_markup (GTK_LABEL (lbl), m->str);
            gtk_label_set_xalign (GTK_LABEL (lbl), 0.0);
            gtk_label_set_yalign (GTK_LABEL (lbl), 0.0);
            gtk_label_set_line_wrap (GTK_LABEL (lbl), TRUE);
            gtk_label_set_line_wrap_mode (GTK_LABEL (lbl), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_selectable (GTK_LABEL (lbl), TRUE);
            gtk_widget_set_halign (lbl, GTK_ALIGN_FILL);
            gtk_widget_set_valign (lbl, GTK_ALIGN_START);
            gtk_widget_show (lbl);

            gtk_container_add (GTK_CONTAINER (box), lbl);
            gtk_style_context_add_class (gtk_widget_get_style_context (box),
                                         "moo-md-blockquote");
            gtk_style_context_add_provider (
                gtk_widget_get_style_context (box),
                GTK_STYLE_PROVIDER (table_css_provider ()),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            gtk_widget_set_halign (box, GTK_ALIGN_FILL);
            gtk_widget_set_valign (box, GTK_ALIGN_START);
            gtk_widget_set_hexpand (box, TRUE);
            gtk_widget_set_vexpand (box, FALSE);
            gtk_widget_set_can_focus (box, FALSE);
            gtk_widget_show (box);

            moo_html_new_line (view, buffer, iter, current, FALSE);
            bq_anchor = gtk_text_buffer_create_child_anchor (buffer, iter);
            gtk_text_view_add_child_at_anchor (view, box, bq_anchor);
            bq_data->rulers = g_slist_prepend (bq_data->rulers, box);
            moo_html_new_line (view, buffer, iter, current, TRUE);

            g_string_free (m, TRUE);
        }

        else if (IS_ELEMENT (child))
        {
            g_message ("unknown node '%s'", child->name);
            process_elm_body (view, buffer, child, current, iter);
        }
        else if (IS_COMMENT (child))
        {
            /* ignore */
        }
        else
        {
            g_warning ("unknown node");
        }
    }
}


static void
process_p_elm (GtkTextView    *view,
               GtkTextBuffer  *buffer,
               xmlNode        *elm,
               MooHtmlTag     *current,
               GtkTextIter    *iter)
{
    /* Force-create a per-paragraph tag carrying pixels-above-lines /
     * pixels-below-lines.  GTK applies those only to the first/last
     * line of a paragraph, so they show up as the visible gap
     * BETWEEN paragraphs — without inflating spacing of every list
     * item / heading / code line which all use the view-level
     * default.  Created with mask=0 + force=TRUE so the tag inherits
     * the parent's style without adding any extra mask attributes. */
    MooHtmlAttr p_attr;
    MooHtmlTag *p_tag;
    memset (&p_attr, 0, sizeof p_attr);
    p_tag = moo_html_create_tag (view, &p_attr, current, TRUE);
    g_object_set (G_OBJECT (p_tag),
                  "pixels-above-lines", 8,
                  "pixels-below-lines", 8,
                  (const char*) NULL);

    moo_html_new_line (view, buffer, iter, p_tag, FALSE);
    process_elm_body (view, buffer, elm, p_tag, iter);
    moo_html_new_line (view, buffer, iter, p_tag, FALSE);
}


/* GitHub-style header slug: lowercase, runs of non-alphanum become a
 * single dash, leading/trailing dashes stripped.  Returned string is
 * owned by caller; NULL if the input has no slug-worthy characters. */
static char *
heading_slugify (const char *text)
{
    GString *s;
    gboolean prev_dash = TRUE;   /* suppress leading dashes */

    if (!text)
        return NULL;

    s = g_string_new (NULL);
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
    /* Strip trailing dash. */
    while (s->len > 0 && s->str[s->len - 1] == '-')
        g_string_truncate (s, s->len - 1);
    if (s->len == 0)
    {
        g_string_free (s, TRUE);
        return NULL;
    }
    return g_string_free (s, FALSE);
}

static void
process_heading_elm (GtkTextView    *view,
                     GtkTextBuffer  *buffer,
                     xmlNode        *elm,
                     MooHtmlTag     *parent,
                     GtkTextIter    *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;
    xmlChar    *id_attr;
    int n;

    g_return_if_fail (elm->name[0] && elm->name[1]);

    n = elm->name[1] - '0';
    g_return_if_fail (1 <= n && n <= 6);

    attr.mask = MOO_HTML_HEADING;
    attr.heading = n;
    current = moo_html_create_tag (view, &attr, parent, FALSE);

    moo_html_new_line (view, buffer, iter, current, FALSE);

    /* Register anchor(s) at the heading's position so [text](#slug)
     * links can resolve.  Two sources, in priority order:
     *   1. An explicit id="..." attribute on the heading element.
     *   2. A GitHub-style slug derived from the heading text.
     * md4c-html doesn't emit ids by default, so the slug path is the
     * one that actually matches Markdown autolinks. */
    id_attr = xmlGetProp (elm, (const xmlChar *) "id");
    if (id_attr)
    {
        moo_html_create_anchor (view, buffer, iter, (const char *) id_attr);
        xmlFree (id_attr);
    }
    {
        xmlChar *txt = xmlNodeGetContent (elm);
        if (txt)
        {
            char *slug = heading_slugify ((const char *) txt);
            if (slug)
            {
                moo_html_create_anchor (view, buffer, iter, slug);
                g_free (slug);
            }
            xmlFree (txt);
        }
    }

    process_elm_body (view, buffer, elm, current, iter);
    moo_html_new_line (view, buffer, iter, current, FALSE);

    /* GitHub renders an <hr>-style rule beneath H1 and H2.  We embed a
     * real GtkSeparator at a child anchor — same trick used by
     * process_hr_elm — so the line is a single full-width pixel rule
     * rather than wrapped text characters. */
    if (n <= 2)
    {
        GtkTextChildAnchor *anchor;
        GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
        MooHtmlData *data = moo_html_get_data (view);
        gtk_widget_show (sep);
        anchor = gtk_text_buffer_create_child_anchor (buffer, iter);
        gtk_text_view_add_child_at_anchor (view, sep, anchor);
        data->rulers = g_slist_prepend (data->rulers, sep);
        moo_html_new_line (view, buffer, iter, parent, TRUE);
    }
}


static void
process_text_node (GtkTextView    *view,
                   GtkTextBuffer  *buffer,
                   xmlNode        *node,
                   MooHtmlTag     *current,
                   GtkTextIter    *iter)
{
    moo_html_insert_text (view, buffer, iter, current, (char*) node->content);
}


#define GET_PROP(elm__,prop__) (xmlGetProp (elm__, (const guchar*) prop__))
#define STRCMP(xm__,normal__) (strcmp ((char*) xm__, normal__))


static void
process_a_elm (GtkTextView    *view,
               GtkTextBuffer  *buffer,
               xmlNode        *elm,
               MooHtmlTag     *parent,
               GtkTextIter    *iter)
{
    xmlChar *href, *name;

    href = GET_PROP (elm, "href");
    name = GET_PROP (elm, "name");

    if (!name)
        name = GET_PROP (elm, "id");

    if (href)
    {
        static MooHtmlAttr attr;
        MooHtmlTag *current;

        attr.mask = MOO_HTML_LINK;
        attr.link = (char*) href;

        current = moo_html_create_tag (view, &attr, parent, FALSE);
        process_elm_body (view, buffer, elm, current, iter);
    }
    else if (name)
    {
        moo_html_create_anchor (view, buffer, iter, (char*) name);
    }

    STR_FREE (href);
    STR_FREE (name);
}


static gboolean
parse_int (const char *str,
           int        *dest)
{
    long num;

    if (!str)
        return FALSE;

    errno = 0;
    num = strtol (str, nullptr, 10);

    if (errno)
        return FALSE;

#if G_MAXINT != G_MAXLONG
    if (num < G_MININT || num > G_MAXINT)
        return FALSE;
#endif

    if (dest)
        *dest = num;

    return TRUE;
}


typedef enum {
    OL_NUM = 0,
    OL_LOWER_ALPHA,
    OL_UPPER_ALPHA,
    OL_LOWER_ROMAN,
    OL_UPPER_ROMAN
} OLType;

static char*
make_li_number (int     count,
                OLType  type)
{
    g_return_val_if_fail (count > 0, nullptr);

    switch (type)
    {
        case OL_UPPER_ROMAN:
        case OL_LOWER_ROMAN:
            g_warning ("implement me");
        case OL_NUM:
            return g_strdup_printf ("%d. ", count);
        case OL_LOWER_ALPHA:
            g_return_val_if_fail (count <= 26, nullptr);
            return g_strdup_printf ("%c. ", count - 1 + 'a');
        case OL_UPPER_ALPHA:
            g_return_val_if_fail (count <= 26, nullptr);
            return g_strdup_printf ("%c. ", count - 1 + 'A');
    }

    g_return_val_if_reached (nullptr);
}

static void
process_ol_elm (GtkTextView    *view,
                GtkTextBuffer  *buffer,
                xmlNode        *elm,
                MooHtmlTag     *current,
                GtkTextIter    *iter)
{
    int count;
    OLType list_type = OL_NUM;
    xmlNode *child;
    xmlChar *start = nullptr, *type = nullptr;
    MooHtmlData *data = moo_html_get_data (view);

    count = 1;
    start = GET_PROP (elm, "start");
    parse_int ((char*) start, &count);

    if ((type = GET_PROP (elm, "type")))
    {
        if (!STRCMP (type, "1"))
            list_type = OL_NUM;
        else if (!STRCMP (type, "a"))
            list_type = OL_LOWER_ALPHA;
        else if (!STRCMP (type, "A"))
            list_type = OL_UPPER_ALPHA;
        else if (!STRCMP (type, "i"))
            list_type = OL_LOWER_ROMAN;
        else if (!STRCMP (type, "I"))
            list_type = OL_UPPER_ROMAN;
        else
        {
            g_warning ("invalid type attribute '%s'",
                       type);
        }
    }

    moo_html_new_line (view, buffer, iter, current, FALSE);

    data->list_depth++;
    for (child = elm->children; child != nullptr; child = child->next)
    {
        if (IS_LI_ELEMENT (child))
        {
            char *number;
            char *prefix;
            gboolean had_new_line;
            xmlChar *value;
            int indent;

            value = GET_PROP (child, "value");
            parse_int ((char*) value, &count);

            number = make_li_number (count, list_type);
            /* Four-space base indent + four more per nesting level
             * (so top-level items sit at column 4 — gives the bullet
             * room to breathe against the left margin and matches
             * the visual offset users expect from a list block). */
            indent = data->list_depth * 4;
            prefix = g_strdup_printf ("%*s%s", indent, "", number);
            had_new_line = data->new_line;

            moo_html_insert_verbatim (view, buffer, iter, current, prefix);
            data->new_line = had_new_line;
            process_elm_body (view, buffer, child, current, iter);
            moo_html_new_line (view, buffer, iter, current, FALSE);
            count++;

            g_free (prefix);
            g_free (number);
            STR_FREE (value);
        }
        else if (IS_TEXT (child) || IS_COMMENT (child))
        {
            /* Whitespace and comments between <li> tags — ignore silently. */
        }
        else if (IS_ELEMENT (child))
        {
            g_message ("unknown node '%s'", child->name);
            process_elm_body (view, buffer, child, current, iter);
        }
    }
    data->list_depth--;

    STR_FREE (start);
    STR_FREE (type);
}


static void
process_ul_elm (GtkTextView    *view,
                GtkTextBuffer  *buffer,
                xmlNode        *elm,
                MooHtmlTag     *current,
                GtkTextIter    *iter)
{
    xmlNode *child;
    MooHtmlData *data = moo_html_get_data (view);

    /* Open the list on its own line, the way <ol> already does.
     * Without this the first bullet runs into the preceding paragraph. */
    moo_html_new_line (view, buffer, iter, current, FALSE);

    /* Each nested <ul>/<ol> bumps list_depth so the corresponding
     * <li> gets a deeper indent + different bullet glyph. */
    data->list_depth++;
    for (child = elm->children; child != nullptr; child = child->next)
    {
        if (IS_LI_ELEMENT (child))
            process_li_elm (view, buffer, child, current, iter);
    }
    data->list_depth--;
}


static void
process_li_elm (GtkTextView    *view,
                GtkTextBuffer  *buffer,
                xmlNode        *elm,
                MooHtmlTag     *current,
                GtkTextIter    *iter)
{
    static const char *bullets[] = { "\xe2\x80\xa2", "\xe2\x97\xa6",
                                     "\xe2\x96\xaa", "\xe2\x80\xa3" };
    gboolean had_new_line;
    MooHtmlData *data = moo_html_get_data (view);
    int depth, indent;
    char *prefix;

    moo_html_new_line (view, buffer, iter, current, FALSE);

    /* list_depth==0 means this <li> was processed outside a <ul>/<ol>
     * (malformed HTML); fall back to a sane default rather than crash. */
    depth = data->list_depth > 0 ? data->list_depth : 1;
    indent = depth * 4;
    prefix = g_strdup_printf ("%*s%s ", indent, "",
                              bullets[(depth - 1) % G_N_ELEMENTS (bullets)]);

    had_new_line = data->new_line;
    moo_html_insert_verbatim (view, buffer, iter, current, prefix);
    data->new_line = had_new_line;

    g_free (prefix);

    process_elm_body (view, buffer, elm, current, iter);
    moo_html_new_line (view, buffer, iter, current, FALSE);
}


static void
process_pre_elm (GtkTextView    *view,
                 GtkTextBuffer  *buffer,
                 xmlNode        *elm,
                 MooHtmlTag     *parent,
                 GtkTextIter    *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;

    attr.mask = MOO_HTML_MONOSPACE | MOO_HTML_PRE;
    current = moo_html_create_tag (view, &attr, parent, FALSE);

    process_elm_body (view, buffer, elm, current, iter);
}


static void
process_font_elm (GtkTextView   *view,
                  GtkTextBuffer *buffer,
                  xmlNode       *elm,
                  MooHtmlTag    *parent,
                  GtkTextIter   *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;
    xmlChar *size__, *color, *face;
    int scale = 0;
    guint size_val;
    const xmlChar *size;

    size__ = GET_PROP (elm, "size");
    color = GET_PROP (elm, "color");
    face = GET_PROP (elm, "face");

    attr.mask = MOO_HTML_NONE;
    attr.font_face = nullptr;

    if (size__)
    {
        size = size__;

        if (size[0] == '+')
        {
            scale = 1;
            size++;
        }
        else if (size[0] == '-')
        {
            scale = -1;
            size++;
        }

        if (!size[0] || size[0] < '1' || size[0] > '7')
        {
            g_warning ("invalid size '%s'", size);
        }
        else
        {
            size_val = size[0] - '1' + 1;

            if (scale == 1)
            {
                attr.mask |= MOO_HTML_LARGER;
                attr.scale = size_val;
            }
            else if (scale == -1)
            {
                attr.mask |= MOO_HTML_SMALLER;
                attr.scale = size_val;
            }
            else
            {
                attr.mask |= MOO_HTML_FONT_SIZE;
                attr.font_size = size_val;
            }
        }
    }

    if (color)
    {
        attr.mask |= MOO_HTML_FG;
        attr.fg = (char*) color;
    }

    if (face && face[0])
    {
        attr.mask |= MOO_HTML_FONT_FACE;
        attr.font_face = (char*) face;
    }

    current = moo_html_create_tag (view, &attr, parent, FALSE);
    process_elm_body (view, buffer, elm, current, iter);

    STR_FREE (size__);
    STR_FREE (color);
    STR_FREE (face);
}


static void
process_cite_elm (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                  MooHtmlTag *parent, GtkTextIter *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;
    current = moo_html_create_tag (view, &attr, parent, FALSE);
    process_elm_body (view, buffer, elm, current, iter);
}


static void
process_format_elm (GtkTextView    *view,
                    MooHtmlAttr    *attr,
                    GtkTextBuffer  *buffer,
                    xmlNode        *elm,
                    MooHtmlTag     *parent,
                    GtkTextIter    *iter)
{
    MooHtmlTag *current;
    current = moo_html_create_tag (view, attr, parent, FALSE);
    process_elm_body (view, buffer, elm, current, iter);
}


static void
process_dt_elm (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                MooHtmlTag *parent, GtkTextIter *iter)
{
    moo_html_new_line (view, buffer, iter, parent, FALSE);
    process_elm_body (view, buffer, elm, parent, iter);
    moo_html_new_line (view, buffer, iter, parent, FALSE);
}


static void
process_dl_elm (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                MooHtmlTag *parent, GtkTextIter *iter)
{
    moo_html_new_line (view, buffer, iter, parent, FALSE);
    process_elm_body (view, buffer, elm, parent, iter);
    moo_html_new_line (view, buffer, iter, parent, FALSE);
}


static void
process_dd_elm (GtkTextView     *view,
                GtkTextBuffer   *buffer,
                xmlNode         *elm,
                MooHtmlTag      *parent,
                GtkTextIter     *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;
    attr.mask = MOO_HTML_LEFT_MARGIN;
    attr.left_margin = 20;
    current = moo_html_create_tag (view, &attr, parent, FALSE);
    moo_html_new_line (view, buffer, iter, current, FALSE);
    process_elm_body (view, buffer, elm, current, iter);
    moo_html_new_line (view, buffer, iter, current, FALSE);
}


static void
process_br_elm (GtkTextView *view,
                GtkTextBuffer *buffer,
                G_GNUC_UNUSED xmlNode *elm,
                MooHtmlTag *parent,
                GtkTextIter *iter)
{
    moo_html_new_line (view, buffer, iter, parent, TRUE);
}


static void
process_div_elm (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                 MooHtmlTag *parent, GtkTextIter *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;
    current = moo_html_create_tag (view, &attr, parent, FALSE);
    process_elm_body (view, buffer, elm, current, iter);
}


static void
process_span_elm (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                  MooHtmlTag *parent, GtkTextIter *iter)
{
    static MooHtmlAttr attr;
    MooHtmlTag *current;
    current = moo_html_create_tag (view, &attr, parent, FALSE);
    process_elm_body (view, buffer, elm, current, iter);
}


/* CSS provider for the horizontal-rule widget.  The default GTK
 * theme-supplied separator is a single faint pixel, easy to miss in
 * a long document; force a more visible thickness here.  The
 * "wikiline-thick" class is used by the wiki preview for 6+-dash
 * rules so they read as a heavier divider.
 *
 * No CSS `margin` here — GtkTextView allocates the widget by its
 * natural size and any extra margin triggers "Negative content
 * height" warnings during size-allocate.  We get visual breathing
 * room around the rule from the surrounding paragraph spacing in
 * the text view instead. */
static GtkCssProvider *
moo_hr_css_provider (void)
{
    static GtkCssProvider *provider = NULL;
    if (provider == NULL)
    {
        provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (provider,
            "separator.moo-hr { "
            "  min-height: 2px; "
            "  background-color: alpha(currentColor, 0.45); "
            "}\n"
            "separator.moo-hr.wikiline-thick { "
            "  min-height: 4px; "
            "  background-color: alpha(currentColor, 0.7); "
            "}\n", -1, NULL);
    }
    return provider;
}

static void
process_hr_elm (GtkTextView *view,
                GtkTextBuffer *buffer,
                xmlNode *elm,
                MooHtmlTag *parent,
                GtkTextIter *iter)
{
    GtkTextChildAnchor *anchor;
    GtkWidget *line;
    MooHtmlData *data = moo_html_get_data (view);

    line = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class (gtk_widget_get_style_context (line),
                                 "moo-hr");
    /* Wiki preview sets class="wikiline-thick" for "------" (6+
     * dashes).  Carry that class through so CSS can paint the
     * thicker variant. */
    xmlChar *cls = xmlGetProp (elm, (const xmlChar *) "class");
    if (cls)
    {
        if (strstr ((const char *) cls, "wikiline-thick"))
            gtk_style_context_add_class (
                gtk_widget_get_style_context (line), "wikiline-thick");
        xmlFree (cls);
    }
    gtk_style_context_add_provider (
        gtk_widget_get_style_context (line),
        GTK_STYLE_PROVIDER (moo_hr_css_provider ()),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_show (line);

    moo_html_new_line (view, buffer, iter, parent, FALSE);

    anchor = gtk_text_buffer_create_child_anchor (buffer, iter);
    gtk_text_view_add_child_at_anchor (GTK_TEXT_VIEW (view), line, anchor);
    data->rulers = g_slist_prepend (data->rulers, line);

    moo_html_new_line (view, buffer, iter, parent, TRUE);
}


static void
process_img_elm (GtkTextView    *view,
                 GtkTextBuffer  *buffer,
                 xmlNode        *elm,
                 MooHtmlTag     *current,
                 GtkTextIter    *iter)
{
    xmlChar *src;
    xmlChar *alt;
    char *path = nullptr;
    GdkPixbuf *pixbuf;
    GError *error = nullptr;
    int offset;
    GtkTextIter before;
    MooHtmlData *data = moo_html_get_data (view);

    src = GET_PROP (elm, "src");
    alt = GET_PROP (elm, "alt");

    g_return_if_fail (src != nullptr);

    if (!data->dirname)
        goto try_alt;

    path = g_build_filename (data->dirname, src, nullptr);
    g_return_if_fail (path != nullptr);

    pixbuf = gdk_pixbuf_new_from_file (path, &error);

    if (!pixbuf)
    {
        g_message ("could not load image '%s': %s", path, moo_error_message (error));
        g_error_free (error);
        goto try_alt;
    }

    offset = gtk_text_iter_get_offset (iter);
    gtk_text_buffer_insert_pixbuf (buffer, iter, pixbuf);
    gtk_text_buffer_get_iter_at_offset (buffer, &before, offset);

    if (current)
        gtk_text_buffer_apply_tag (buffer, GTK_TEXT_TAG (current), &before, iter);

    g_object_unref (pixbuf);
    goto out;

try_alt:
    if (alt)
    {
        char *text = g_strdup_printf ("[%s]", alt);
        moo_html_insert_text (view, buffer, iter, current, text);
        g_free (text);
    }

out:
    STR_FREE (src);
    STR_FREE (alt);
    g_free (path);
}


/* ─────────────── Table rendering (2-pass column-aligned) ───────────────
 *
 * GtkTextView can't draw a real grid, so we approximate one with
 * monospace text and Unicode box-drawing characters.  First pass walks
 * the table tree, collects each <tr>'s text-content cells, and computes
 * the max char-width per column.  Second pass emits each row with
 * cells padded to the column width and a header separator after the
 * <thead> row.  The whole block is wrapped in a MOO_HTML_TABLE tag so
 * the Markdown preview plugin can apply monospace font + soft
 * background.
 */

typedef struct {
    char *text;       /* pango markup; g_free-owned */
    int   colspan;    /* >= 1 */
    int   rowspan;    /* >= 1 */
    gboolean is_header;
} TableCell;

typedef struct {
    GPtrArray *cells;     /* TableCell* per cell */
    gboolean   is_header; /* came from <thead> or had any <th> */
} TableRow;

static void
table_cell_free (gpointer p)
{
    TableCell *c = (TableCell *) p;
    if (!c) return;
    g_free (c->text);
    g_free (c);
}

static void
table_row_free (gpointer p)
{
    TableRow *r = (TableRow *) p;
    if (r->cells)
        g_ptr_array_free (r->cells, TRUE);
    g_free (r);
}

/* Walk a cell node's children and emit Pango markup so that simple
 * inline HTML (<code>, <strong>, <em>, <a>) survives into the
 * GtkLabel.  Anything we don't recognise falls through as plain text.
 * The result is owned by caller (g_free). */
static void
xml_cell_to_pango (xmlNode *elm, GString *out)
{
    for (xmlNode *c = elm->children; c != nullptr; c = c->next)
    {
        if (IS_TEXT (c))
        {
            char *esc = g_markup_escape_text ((const char *) c->content, -1);
            g_string_append (out, esc);
            g_free (esc);
        }
        else if (!IS_ELEMENT (c))
        {
            continue;
        }
        else if (IS_NAMED_ELM_ (c, "code") || IS_NAMED_ELM_ (c, "tt"))
        {
            g_string_append (out, "<tt>");
            xml_cell_to_pango (c, out);
            g_string_append (out, "</tt>");
        }
        else if (IS_NAMED_ELM_ (c, "strong") || IS_NAMED_ELM_ (c, "b"))
        {
            g_string_append (out, "<b>");
            xml_cell_to_pango (c, out);
            g_string_append (out, "</b>");
        }
        else if (IS_NAMED_ELM_ (c, "em") || IS_NAMED_ELM_ (c, "i"))
        {
            g_string_append (out, "<i>");
            xml_cell_to_pango (c, out);
            g_string_append (out, "</i>");
        }
        else if (IS_NAMED_ELM_ (c, "u"))
        {
            g_string_append (out, "<u>");
            xml_cell_to_pango (c, out);
            g_string_append (out, "</u>");
        }
        else if (IS_NAMED_ELM_ (c, "a"))
        {
            xmlChar *href = xmlGetProp (c, (const xmlChar *) "href");
            if (href)
            {
                char *esc = g_markup_escape_text ((const char *) href, -1);
                g_string_append_printf (out,
                    "<span foreground=\"#1a73e8\" underline=\"single\">");
                xml_cell_to_pango (c, out);
                g_string_append (out, "</span>");
                g_free (esc);
                xmlFree (href);
            }
            else
            {
                xml_cell_to_pango (c, out);
            }
        }
        else
        {
            /* Unknown inline element — emit its text content verbatim. */
            xml_cell_to_pango (c, out);
        }
    }
}

/* Collect <tr> rows under `elm`, recursing through <thead>/<tbody>/<tfoot>.
 * inside_thead tracks whether we're descended from a <thead>. */
static void
table_collect_rows (xmlNode *elm, GPtrArray *rows, gboolean inside_thead)
{
    xmlNode *child;
    for (child = elm->children; child != nullptr; child = child->next)
    {
        if (!IS_ELEMENT (child))
            continue;
        if (IS_NAMED_ELM_ (child, "tr"))
        {
            TableRow *row = g_new0 (TableRow, 1);
            row->cells     = g_ptr_array_new_with_free_func (table_cell_free);
            row->is_header = inside_thead;

            for (xmlNode *c = child->children; c != nullptr; c = c->next)
            {
                if (!IS_ELEMENT (c))
                    continue;
                if (IS_NAMED_ELM_ (c, "th"))
                    row->is_header = TRUE;
                if (IS_NAMED_ELM_ (c, "td") || IS_NAMED_ELM_ (c, "th"))
                {
                    GString *m = g_string_new (NULL);
                    xml_cell_to_pango (c, m);
                    g_strstrip (m->str);

                    TableCell *cell = g_new0 (TableCell, 1);
                    cell->text      = g_string_free (m, FALSE);
                    cell->colspan   = 1;
                    cell->rowspan   = 1;
                    cell->is_header = IS_NAMED_ELM_ (c, "th") || inside_thead;

                    xmlChar *cs = xmlGetProp (c, (const xmlChar *) "colspan");
                    if (cs)
                    {
                        int v = atoi ((const char *) cs);
                        if (v > 1) cell->colspan = v;
                        xmlFree (cs);
                    }
                    xmlChar *rs = xmlGetProp (c, (const xmlChar *) "rowspan");
                    if (rs)
                    {
                        int v = atoi ((const char *) rs);
                        if (v > 1) cell->rowspan = v;
                        xmlFree (rs);
                    }

                    g_ptr_array_add (row->cells, cell);
                }
            }
            g_ptr_array_add (rows, row);
        }
        else if (IS_NAMED_ELM_ (child, "thead"))
            table_collect_rows (child, rows, TRUE);
        else if (IS_NAMED_ELM_ (child, "tbody")
                 || IS_NAMED_ELM_ (child, "tfoot"))
            table_collect_rows (child, rows, FALSE);
    }
}

/* CSS applied to every embedded table widget.  Cell borders collapse
 * by giving each cell border-top + border-left and the table itself
 * border-right + border-bottom, but GTK's CSS doesn't do collapsing
 * properly across grid children, so we just put a full 1 px border on
 * every cell and accept the 2-px-thick interior lines.  Header cells
 * get a slightly darker background. */
static const char TABLE_CSS[] =
    /* min-height on the grid + cells prevents GTK from running a
     * size-allocate pass where allocation < (border+padding), which
     * produces "Negative content height" warnings during early
     * layout in GtkTextView. */
    "grid.moo-md-table { padding: 0; margin: 4px 0; min-height: 24px; }\n"
    "grid.moo-md-table > label { "
    "  padding: 4px 10px; "
    "  min-height: 16px; "
    "  min-width: 8px; "
    "  border: 1px solid alpha(currentColor, 0.35); "
    "}\n"
    "grid.moo-md-table > label.moo-md-th { "
    "  font-weight: bold; "
    "  background: alpha(currentColor, 0.08); "
    "}\n"
    /* Blockquote: continuous left bar across all wrapped lines via
     * border-left on the container.  font-style on the inner label
     * gives the italic look. */
    ".moo-md-blockquote { "
    "  border-left: 4px solid alpha(currentColor, 0.35); "
    "  background: alpha(currentColor, 0.06); "
    /* 8 pt left gap between the border and the text (≈ 11 px),
     * matching the user-visible "padding: 8pt" Habitat ships with.
     * Vertical padding stays smaller so the block doesn't grow tall
     * for short single-paragraph quotes. */
    "  padding: 8px 8pt 8px 8pt; "
    "  margin: 6px 0; "
    "  min-height: 24px; "
    "}\n"
    ".moo-md-blockquote label { "
    "  font-style: italic; "
    "  color: alpha(currentColor, 0.75); "
    "}\n";

static GtkCssProvider *
table_css_provider (void)
{
    static GtkCssProvider *provider = NULL;
    if (provider == NULL)
    {
        provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (provider, TABLE_CSS, -1, NULL);
    }
    return provider;
}

static void
process_table_elm (GtkTextView *view,
                   GtkTextBuffer *buffer,
                   xmlNode *elm,
                   MooHtmlTag *parent,
                   GtkTextIter *iter)
{
    GPtrArray          *rows;
    GtkWidget          *grid;
    GtkTextChildAnchor *anchor;
    MooHtmlData        *data;
    guint               ncols = 0;

    rows = g_ptr_array_new_with_free_func (table_row_free);
    table_collect_rows (elm, rows, FALSE);

    if (rows->len == 0)
    {
        g_ptr_array_free (rows, TRUE);
        return;
    }

    /* Column count = max of (sum of colspans in row) across rows. */
    for (guint r = 0; r < rows->len; r++)
    {
        TableRow *row = (TableRow *) rows->pdata[r];
        guint sum = 0;
        for (guint i = 0; i < row->cells->len; i++)
        {
            TableCell *cell = (TableCell *) row->cells->pdata[i];
            sum += cell->colspan;
        }
        if (sum > ncols)
            ncols = sum;
    }
    if (ncols == 0)
    {
        g_ptr_array_free (rows, TRUE);
        return;
    }

    /* Build the grid: one GtkLabel per cell, attached at (col, row)
     * with width/height taken from colspan/rowspan.  A `covered` map
     * tracks (col, row) positions already claimed by a rowspan from
     * an earlier row so the next cell skips over them. */
    grid = gtk_grid_new ();
    gtk_style_context_add_class (gtk_widget_get_style_context (grid),
                                 "moo-md-table");
    gtk_style_context_add_provider (gtk_widget_get_style_context (grid),
                                    GTK_STYLE_PROVIDER (table_css_provider ()),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_grid_set_row_spacing (GTK_GRID (grid), 0);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 0);
    gtk_widget_set_halign (grid, GTK_ALIGN_START);
    gtk_widget_set_valign (grid, GTK_ALIGN_START);
    gtk_widget_set_hexpand (grid, FALSE);
    gtk_widget_set_vexpand (grid, FALSE);
    gtk_widget_set_can_focus (grid, FALSE);

    /* covered[row * ncols + col] — TRUE iff that slot is claimed by
     * a rowspan from an earlier row.  We never need to mark slots in
     * the *current* row from cells in the same row, since cells in a
     * row are emitted left-to-right and the colspan just bumps the
     * next-cell column counter. */
    gboolean *covered = g_new0 (gboolean, rows->len * ncols);

    for (guint r = 0; r < rows->len; r++)
    {
        TableRow *row = (TableRow *) rows->pdata[r];
        guint     col = 0;
        for (guint i = 0; i < row->cells->len; i++)
        {
            TableCell *cell = (TableCell *) row->cells->pdata[i];

            /* Skip slots already taken by an earlier row's rowspan. */
            while (col < ncols && covered[r * ncols + col])
                col++;
            if (col >= ncols)
                break;

            int width  = cell->colspan;
            int height = cell->rowspan;
            if (col + width > ncols)
                width = ncols - col;
            if (r + height > rows->len)
                height = rows->len - r;

            GtkWidget *label = gtk_label_new (NULL);
            gtk_label_set_markup (GTK_LABEL (label),
                                  cell->text ? cell->text : "");
            gtk_label_set_xalign (GTK_LABEL (label), 0.0);
            gtk_label_set_yalign (GTK_LABEL (label), 0.5);
            gtk_widget_set_halign (label, GTK_ALIGN_FILL);
            gtk_widget_set_valign (label, GTK_ALIGN_FILL);
            gtk_widget_set_hexpand (label, TRUE);
            gtk_widget_set_vexpand (label, FALSE);
            gtk_label_set_selectable (GTK_LABEL (label), TRUE);
            if (cell->is_header || row->is_header)
                gtk_style_context_add_class (
                    gtk_widget_get_style_context (label), "moo-md-th");
            gtk_style_context_add_provider (
                gtk_widget_get_style_context (label),
                GTK_STYLE_PROVIDER (table_css_provider ()),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            gtk_widget_show (label);
            gtk_grid_attach (GTK_GRID (grid), label,
                             col, r, width, height);

            /* Mark every (row, col) slot the cell spans as covered. */
            for (int dr = 0; dr < height; dr++)
                for (int dc = 0; dc < width; dc++)
                    covered[(r + dr) * ncols + (col + dc)] = TRUE;

            col += width;
        }
    }
    g_free (covered);
    gtk_widget_show (grid);

    /* Embed the grid into the text view at a child anchor.  Tracked via
     * data->rulers so the buffer cleanup tears it down. */
    data = moo_html_get_data (view);
    moo_html_new_line (view, buffer, iter, parent, FALSE);
    anchor = gtk_text_buffer_create_child_anchor (buffer, iter);
    gtk_text_view_add_child_at_anchor (view, grid, anchor);
    data->rulers = g_slist_prepend (data->rulers, grid);
    moo_html_new_line (view, buffer, iter, parent, TRUE);

    g_ptr_array_free (rows, TRUE);
}

static void
process_tr_elm (GtkTextView *view, GtkTextBuffer *buffer, xmlNode *elm,
                MooHtmlTag *parent, GtkTextIter *iter)
{
    /* No-op: process_table_elm fully owns the table render, so a stray
     * <tr> outside a <table> is silently dropped.  Real rows are walked
     * by table_collect_rows, not via this dispatch path. */
    (void) view; (void) buffer; (void) elm; (void) parent; (void) iter;
}


/* ───────────── Tag-kind predicates (public via moohtml.h) ────────────
 *
 * Consumers like the Markdown preview pane need to restyle tags after
 * _moo_html_load_memory() returns.  The bitmask lives in the private
 * MooHtmlAttr struct (definition inside this TU), so expose narrow
 * accessors instead of leaking the struct shape.
 */

gboolean
_moo_html_tag_is_link (GtkTextTag *tag)
{
    if (!MOO_IS_HTML_TAG (tag))
        return FALSE;
    MooHtmlTag *t = MOO_HTML_TAG (tag);
    return t->attr && (t->attr->mask & MOO_HTML_LINK) != 0;
}

gboolean
_moo_html_tag_is_monospace (GtkTextTag *tag)
{
    if (!MOO_IS_HTML_TAG (tag))
        return FALSE;
    MooHtmlTag *t = MOO_HTML_TAG (tag);
    return t->attr && (t->attr->mask & MOO_HTML_MONOSPACE) != 0;
}

gboolean
_moo_html_tag_is_pre (GtkTextTag *tag)
{
    if (!MOO_IS_HTML_TAG (tag))
        return FALSE;
    MooHtmlTag *t = MOO_HTML_TAG (tag);
    return t->attr && (t->attr->mask & MOO_HTML_PRE) != 0;
}

int
_moo_html_tag_get_heading (GtkTextTag *tag)
{
    if (!MOO_IS_HTML_TAG (tag))
        return 0;
    MooHtmlTag *t = MOO_HTML_TAG (tag);
    if (!t->attr || !(t->attr->mask & MOO_HTML_HEADING))
        return 0;
    return (int) t->attr->heading;   /* 1..6 */
}

gboolean
_moo_html_tag_is_blockquote (GtkTextTag *tag)
{
    if (!MOO_IS_HTML_TAG (tag))
        return FALSE;
    MooHtmlTag *t = MOO_HTML_TAG (tag);
    return t->attr && (t->attr->mask & MOO_HTML_BLOCKQUOTE) != 0;
}

gboolean
_moo_html_tag_is_table (GtkTextTag *tag)
{
    if (!MOO_IS_HTML_TAG (tag))
        return FALSE;
    MooHtmlTag *t = MOO_HTML_TAG (tag);
    return t->attr && (t->attr->mask & MOO_HTML_TABLE) != 0;
}
