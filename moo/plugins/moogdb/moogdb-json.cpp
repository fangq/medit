/*
 *   moogdb-json.cpp
 *
 *   Hand-rolled JSON parser.  Recursive descent over a UTF-8 byte
 *   stream with a one-character pushback (via index arithmetic).
 *   Tolerates trailing commas and line/block comments so the
 *   user-edited VS Code launch.json variants parse cleanly.
 *
 *   This file is part of medit.  medit is free software; you can
 *   redistribute it and/or modify it under the terms of the
 *   GNU Lesser General Public License as published by the
 *   Free Software Foundation; either version 2.1 of the License,
 *   or (at your option) any later version.
 */

#include "plugins/moogdb/moogdb-json.h"
#include <string.h>
#include <stdlib.h>

#define MOO_JSON_ERROR (moo_json_error_quark ())
static GQuark moo_json_error_quark (void)
    { return g_quark_from_static_string ("moo-json-error-quark"); }

struct _MooJsonValue {
    MooJsonKind kind;
    union {
        gboolean   b;
        double     n;
        char      *s;
        GPtrArray *arr;     /* MooJsonValue* */
        GPtrArray *obj_keys;   /* char* */
        /* parallel: obj_vals at same length */
    } v;
    GPtrArray *obj_vals;   /* only used when kind == MOO_JSON_OBJECT */
};

typedef struct {
    const char *src;
    const char *p;        /* current cursor */
    int         line;
    int         col;
    int         depth;    /* nesting depth, to bound recursion */
    GError    **error;
} ParseState;

/* launch.json files are shallow; the limit only exists so a pathological or
 * truncated file ("[[[[[[...") cannot recurse the stack to death. */
#define JSON_MAX_DEPTH 100

static void
set_error (ParseState *ps, const char *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
set_error (ParseState *ps, const char *fmt, ...)
{
    if (!ps->error || *ps->error) return;
    va_list ap;
    va_start (ap, fmt);
    char *msg = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    g_set_error (ps->error, MOO_JSON_ERROR, 0,
                  "line %d col %d: %s", ps->line, ps->col, msg);
    g_free (msg);
}

static void
advance (ParseState *ps)
{
    char c = *ps->p++;
    if (c == '\n') { ps->line++; ps->col = 1; }
    else ps->col++;
}

static void
skip_ws_and_comments (ParseState *ps)
{
    for (;;) {
        while (*ps->p && g_ascii_isspace (*ps->p))
            advance (ps);
        if (ps->p[0] == '/' && ps->p[1] == '/') {
            while (*ps->p && *ps->p != '\n') advance (ps);
            continue;
        }
        if (ps->p[0] == '/' && ps->p[1] == '*') {
            advance (ps); advance (ps);
            while (*ps->p && !(ps->p[0] == '*' && ps->p[1] == '/'))
                advance (ps);
            if (*ps->p) { advance (ps); advance (ps); }
            continue;
        }
        break;
    }
}

static MooJsonValue *parse_value (ParseState *ps);

static MooJsonValue *
new_value (MooJsonKind k)
{
    MooJsonValue *v = g_new0 (MooJsonValue, 1);
    v->kind = k;
    return v;
}

static void
free_value_cb (gpointer p) { moo_json_value_free ((MooJsonValue *) p); }

void
moo_json_value_free (MooJsonValue *v)
{
    if (!v) return;
    switch (v->kind) {
    case MOO_JSON_STRING:
        g_free (v->v.s);
        break;
    case MOO_JSON_ARRAY:
        if (v->v.arr) g_ptr_array_free (v->v.arr, TRUE);
        break;
    case MOO_JSON_OBJECT:
        if (v->v.obj_keys) g_ptr_array_free (v->v.obj_keys, TRUE);
        if (v->obj_vals)   g_ptr_array_free (v->obj_vals,   TRUE);
        break;
    default: break;
    }
    g_free (v);
}

/* JSON string: surrounded by ", with backslash escapes \"\\\/\b\f
 * \n\r\t\u<4hex>.  We decode to UTF-8 (the \u escapes go through
 * g_unichar_to_utf8). */
static char *
parse_string (ParseState *ps)
{
    if (*ps->p != '"') { set_error (ps, "expected '\"'"); return NULL; }
    advance (ps);
    GString *out = g_string_new (NULL);
    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            advance (ps);
            char esc = *ps->p;
            if (!esc) { set_error (ps, "unterminated escape");
                        g_string_free (out, TRUE); return NULL; }
            switch (esc) {
            case '"':  g_string_append_c (out, '"');  advance (ps); break;
            case '\\': g_string_append_c (out, '\\'); advance (ps); break;
            case '/':  g_string_append_c (out, '/');  advance (ps); break;
            case 'b':  g_string_append_c (out, '\b'); advance (ps); break;
            case 'f':  g_string_append_c (out, '\f'); advance (ps); break;
            case 'n':  g_string_append_c (out, '\n'); advance (ps); break;
            case 'r':  g_string_append_c (out, '\r'); advance (ps); break;
            case 't':  g_string_append_c (out, '\t'); advance (ps); break;
            case 'u': {
                advance (ps);
                char hex[5] = {0};
                for (int i = 0; i < 4; i++) {
                    if (!g_ascii_isxdigit (*ps->p)) {
                        set_error (ps, "bad \\u escape");
                        g_string_free (out, TRUE); return NULL;
                    }
                    hex[i] = *ps->p;
                    advance (ps);
                }
                gunichar uc = (gunichar) strtoul (hex, NULL, 16);

                /* A high surrogate must be followed by "\uDC00-\uDFFF";
                   together they encode one character above the BMP.  Emitting
                   either half on its own yields CESU-8, which GTK rejects
                   later on with "Invalid UTF-8". */
                if (uc >= 0xD800 && uc <= 0xDBFF) {
                    if (ps->p[0] == '\\' && ps->p[1] == 'u') {
                        char lo_hex[5] = {0};
                        const char *save = ps->p;
                        advance (ps); advance (ps);
                        int i;
                        for (i = 0; i < 4 && g_ascii_isxdigit (*ps->p); i++) {
                            lo_hex[i] = *ps->p;
                            advance (ps);
                        }
                        gunichar lo = (i == 4)
                            ? (gunichar) strtoul (lo_hex, NULL, 16) : 0;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            uc = 0x10000 + ((uc - 0xD800) << 10)
                                         + (lo - 0xDC00);
                        } else {
                            ps->p = save;   /* not a pair; report below */
                            uc = 0;
                        }
                    } else {
                        uc = 0;
                    }
                }

                if (uc == 0 || (uc >= 0xDC00 && uc <= 0xDFFF) ||
                    !g_unichar_validate (uc)) {
                    set_error (ps, "invalid \\u escape");
                    g_string_free (out, TRUE);
                    return NULL;
                }

                char utf8[8] = {0};
                int n = g_unichar_to_utf8 (uc, utf8);
                g_string_append_len (out, utf8, n);
                break;
            }
            default:
                set_error (ps, "unknown escape '\\%c'", esc);
                g_string_free (out, TRUE);
                return NULL;
            }
        } else {
            g_string_append_c (out, *ps->p);
            advance (ps);
        }
    }
    if (*ps->p != '"') {
        set_error (ps, "unterminated string");
        g_string_free (out, TRUE);
        return NULL;
    }
    advance (ps);
    return g_string_free (out, FALSE);
}

static MooJsonValue *
parse_number (ParseState *ps)
{
    const char *start = ps->p;
    if (*ps->p == '-') advance (ps);
    while (g_ascii_isdigit (*ps->p)) advance (ps);
    if (*ps->p == '.') {
        advance (ps);
        while (g_ascii_isdigit (*ps->p)) advance (ps);
    }
    if (*ps->p == 'e' || *ps->p == 'E') {
        advance (ps);
        if (*ps->p == '+' || *ps->p == '-') advance (ps);
        while (g_ascii_isdigit (*ps->p)) advance (ps);
    }
    char *buf = g_strndup (start, ps->p - start);
    MooJsonValue *v = new_value (MOO_JSON_NUMBER);
    v->v.n = g_ascii_strtod (buf, NULL);
    g_free (buf);
    return v;
}

static MooJsonValue *
parse_literal (ParseState *ps, const char *word, MooJsonKind k, gboolean b)
{
    size_t n = strlen (word);
    if (strncmp (ps->p, word, n) != 0) {
        set_error (ps, "expected literal '%s'", word);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) advance (ps);
    MooJsonValue *v = new_value (k);
    v->v.b = b;
    return v;
}

static MooJsonValue *
parse_array (ParseState *ps)
{
    advance (ps);   /* eat '[' */
    MooJsonValue *v = new_value (MOO_JSON_ARRAY);
    v->v.arr = g_ptr_array_new_with_free_func (free_value_cb);
    skip_ws_and_comments (ps);
    if (*ps->p == ']') { advance (ps); return v; }
    for (;;) {
        MooJsonValue *child = parse_value (ps);
        if (!child) { moo_json_value_free (v); return NULL; }
        g_ptr_array_add (v->v.arr, child);
        skip_ws_and_comments (ps);
        if (*ps->p == ',') {
            advance (ps);
            skip_ws_and_comments (ps);
            if (*ps->p == ']') { advance (ps); return v; }   /* trailing comma */
            continue;
        }
        if (*ps->p == ']') { advance (ps); return v; }
        set_error (ps, "expected ',' or ']' in array");
        moo_json_value_free (v);
        return NULL;
    }
}

static MooJsonValue *
parse_object (ParseState *ps)
{
    advance (ps);   /* eat '{' */
    MooJsonValue *v = new_value (MOO_JSON_OBJECT);
    v->v.obj_keys = g_ptr_array_new_with_free_func (g_free);
    v->obj_vals   = g_ptr_array_new_with_free_func (free_value_cb);
    skip_ws_and_comments (ps);
    if (*ps->p == '}') { advance (ps); return v; }
    for (;;) {
        skip_ws_and_comments (ps);
        char *key = parse_string (ps);
        if (!key) { moo_json_value_free (v); return NULL; }
        skip_ws_and_comments (ps);
        if (*ps->p != ':') {
            set_error (ps, "expected ':' after key");
            g_free (key);
            moo_json_value_free (v);
            return NULL;
        }
        advance (ps);
        MooJsonValue *val = parse_value (ps);
        if (!val) { g_free (key); moo_json_value_free (v); return NULL; }
        g_ptr_array_add (v->v.obj_keys, key);
        g_ptr_array_add (v->obj_vals,   val);
        skip_ws_and_comments (ps);
        if (*ps->p == ',') {
            advance (ps);
            skip_ws_and_comments (ps);
            if (*ps->p == '}') { advance (ps); return v; }   /* trailing comma */
            continue;
        }
        if (*ps->p == '}') { advance (ps); return v; }
        set_error (ps, "expected ',' or '}' in object");
        moo_json_value_free (v);
        return NULL;
    }
}

static MooJsonValue *
parse_value (ParseState *ps)
{
    skip_ws_and_comments (ps);
    char c = *ps->p;
    if (c == '"') {
        char *s = parse_string (ps);
        if (!s) return NULL;
        MooJsonValue *v = new_value (MOO_JSON_STRING);
        v->v.s = s;
        return v;
    }
    if (c == '{' || c == '[') {
        /* Bounded here rather than in parse_object/parse_array, each of which
           has several return paths; this is the only way into them. */
        if (ps->depth >= JSON_MAX_DEPTH) {
            set_error (ps, "maximum nesting depth (%d) exceeded",
                       JSON_MAX_DEPTH);
            return NULL;
        }
        ps->depth++;
        MooJsonValue *v = (c == '{') ? parse_object (ps) : parse_array (ps);
        ps->depth--;
        return v;
    }
    if (c == '-' || g_ascii_isdigit (c)) return parse_number (ps);
    if (!strncmp (ps->p, "true",  4))
        return parse_literal (ps, "true",  MOO_JSON_BOOL, TRUE);
    if (!strncmp (ps->p, "false", 5))
        return parse_literal (ps, "false", MOO_JSON_BOOL, FALSE);
    if (!strncmp (ps->p, "null",  4))
        return parse_literal (ps, "null",  MOO_JSON_NULL, FALSE);
    set_error (ps, "unexpected character '%c' (0x%02x)",
                g_ascii_isprint (c) ? c : '?', (unsigned char) c);
    return NULL;
}

MooJsonValue *
moo_json_parse (const char *text, GError **error)
{
    g_return_val_if_fail (text != NULL, NULL);
    ParseState ps;
    ps.src   = text;
    ps.p     = text;
    ps.line  = 1;
    ps.col   = 1;
    ps.depth = 0;
    ps.error = error;
    MooJsonValue *v = parse_value (&ps);
    if (!v) return NULL;
    skip_ws_and_comments (&ps);
    if (*ps.p) {
        set_error (&ps, "trailing junk after document");
        moo_json_value_free (v);
        return NULL;
    }
    return v;
}

MooJsonKind  moo_json_kind   (MooJsonValue *v) { return v ? v->kind : MOO_JSON_NULL; }
gboolean     moo_json_bool   (MooJsonValue *v)
    { return v && v->kind == MOO_JSON_BOOL   ? v->v.b : FALSE; }
double       moo_json_number (MooJsonValue *v)
    { return v && v->kind == MOO_JSON_NUMBER ? v->v.n : 0.0; }
const char  *moo_json_string (MooJsonValue *v)
    { return v && v->kind == MOO_JSON_STRING ? v->v.s : NULL; }

MooJsonValue *
moo_json_object_get (MooJsonValue *v, const char *key)
{
    if (!v || v->kind != MOO_JSON_OBJECT || !key) return NULL;
    for (guint i = 0; i < v->v.obj_keys->len; i++) {
        const char *k = (const char *) v->v.obj_keys->pdata[i];
        if (!strcmp (k, key))
            return (MooJsonValue *) v->obj_vals->pdata[i];
    }
    return NULL;
}

guint
moo_json_array_len (MooJsonValue *v)
{
    if (!v || v->kind != MOO_JSON_ARRAY) return 0;
    return v->v.arr->len;
}

MooJsonValue *
moo_json_array_get (MooJsonValue *v, guint index)
{
    if (!v || v->kind != MOO_JSON_ARRAY || index >= v->v.arr->len) return NULL;
    return (MooJsonValue *) v->v.arr->pdata[index];
}

const char *
moo_json_object_string (MooJsonValue *v, const char *key, const char *fallback)
{
    MooJsonValue *child = moo_json_object_get (v, key);
    if (!child || child->kind != MOO_JSON_STRING) return fallback;
    return child->v.s;
}
