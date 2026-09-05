/*
 *   moo-pygobject.c
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

#include <Python.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "moopython/pygobject/moo-pygobject.h"
#include "moopython/moopython-utils.h"
#include <pygobject.h>
#include <mooglib/moo-glib.h>
#include "moopython/moopython-pygobjectmod.h"
#include <mooutils/moostock.h>

/* Symbols defined in the generated moo-mod.cpp (extern "C" linkage) */
extern const PyMethodDef _moo_functions[];
extern void _moo_add_constants(PyObject *module, const gchar *strip_prefix);
extern void _moo_register_classes(PyObject *d);

/**
 * moo_window_class_add_action: (moo.lua 0) (moo.private 1)
 **/

static void     init_moo_utils          (PyObject       *module);

static PyObject *
moo_version (void)
{
    /* Python 3: PyString_FromString -> PyUnicode_FromString */
    return PyUnicode_FromString (MOO_VERSION);
}

static PyObject *
moo_detailed_version (void)
{
    PyObject *res = PyDict_New ();
    g_return_val_if_fail (res != NULL, NULL);

    /* Python 3: PyString_FromString -> PyUnicode_FromString, PyInt_FromLong -> PyLong_FromLong */
    PyDict_SetItemString (res, "full", PyUnicode_FromString (MOO_VERSION));
    PyDict_SetItemString (res, "major", PyLong_FromLong (MOO_MAJOR_VERSION));
    PyDict_SetItemString (res, "minor", PyLong_FromLong (MOO_MINOR_VERSION));
    PyDict_SetItemString (res, "micro", PyLong_FromLong (MOO_MICRO_VERSION));

    return res;
}

static const char _moo_module_doc[] = "_moo module.";

G_GNUC_UNUSED static PyObject *
py_object_from_moo_py_object (const GValue *value)
{
    PyObject *obj;

    g_return_val_if_fail (G_VALUE_TYPE (value) == MOO_TYPE_PY_OBJECT, NULL);

    obj = g_value_get_boxed (value);

    if (!obj)
        obj = Py_None;

    return _moo_py_object_ref (obj);
}

G_GNUC_UNUSED static int
py_object_to_moo_py_object (GValue *value, PyObject *obj)
{
    g_value_set_boxed (value, obj == Py_None ? obj : NULL);
    return 0;
}

/* Python 3 module initialization structure */
static struct PyModuleDef _moo_moduledef = {
    PyModuleDef_HEAD_INIT,
    "_moo",                     /* m_name */
    _moo_module_doc,           /* m_doc */
    -1,                        /* m_size */
    (PyMethodDef*) _moo_functions, /* m_methods */
    NULL,                      /* m_reload */
    NULL,                      /* m_traverse */
    NULL,                      /* m_clear */
    NULL,                      /* m_free */
};





/* ================================================================ */
/* Pre-register parent GTypes in pygobject's type map.              */
/* In PyGObject 3.x, types are registered lazily when imported from */
/* gi.repository. We must trigger this before registering Moo types */
/* so pygobject_register_class() can find the parent types.         */
/* ================================================================ */
static void
_moo_preregister_parent_gtypes(void)
{
    /* Import gi with proper version requirements, then import Gtk.
     * This causes PyGObject to register all Gtk GTypes internally,
     * so when we call pygobject_register_class() for MooEdit (whose
     * GType parent is GtkTextView), pygobject can find GtkTextView
     * in its type map. */
    const char *preregister_script =
        "import gi\n"
        "gi.require_version('Gtk', '3.0')\n"
        "gi.require_version('GdkPixbuf', '2.0')\n"
        "from gi.repository import GObject, Gio, Gtk, GdkPixbuf\n"
        "# Force registration of specific types we need as parents\n"
        "_ = [\n"
        "    Gtk.Widget, Gtk.Container, Gtk.Bin, Gtk.Window,\n"
        "    Gtk.Dialog, Gtk.Frame, Gtk.Table, Gtk.Entry,\n"
        "    Gtk.Notebook, Gtk.VBox, Gtk.TextView, Gtk.TextBuffer,\n"
        "    Gtk.ToggleToolButton, Gtk.Action,\n"
        "    GObject.Object, Gio.File, GdkPixbuf.Pixbuf,\n"
        "]\n"
        "# Also register via pygobject's internal mechanism\n"
        "for cls in _:\n"
        "    try:\n"
        "        cls.__gtype__\n"  
        "    except:\n"
        "        pass\n";


    if (PyRun_SimpleString(preregister_script) != 0) {
        g_warning("_moo_preregister_parent_gtypes: failed to import gi types");
        if (PyErr_Occurred())
            PyErr_Print();
    }

}

gboolean
_moo_module_init (void)
{
    static gboolean initialized = FALSE;
    PyObject *_moo_module = NULL;
    PyObject *modules;

    if (initialized)
        return TRUE;

    init_pygobject_mod ();
    if (PyErr_Occurred ())
        return FALSE;
    /* Resolve real GTK PyTypeObjects from gi.repository */
    _moo_preregister_parent_gtypes();


    _moo_module = PyModule_Create (&_moo_moduledef);
    if (!_moo_module)
        return FALSE;

    /* Register in sys.modules so "import _moo" works */
    modules = PyImport_GetModuleDict ();
    PyDict_SetItemString (modules, "_moo", _moo_module);

    PyModule_AddObject (_moo_module, "version", moo_version());
    if (PyErr_Occurred ()) PyErr_Clear ();

    PyModule_AddObject (_moo_module, "detailed_version", moo_detailed_version());
    if (PyErr_Occurred ()) PyErr_Clear ();

    init_moo_utils (_moo_module);
    if (PyErr_Occurred ()) PyErr_Clear ();

    _moo_add_constants (_moo_module, "MOO_");
    if (PyErr_Occurred ()) PyErr_Clear ();

    /* GTK3: Pre-import gi.repository.Gtk so PyGObject initializes
     * proper GTK types before _moo registers its custom types */
    PyRun_SimpleString(
        "import gi\n"
        "gi.require_version('Gtk', '3.0')\n"
        "gi.require_version('Gdk', '3.0')\n"
        "from gi.repository import Gtk\n"
    );
    if (PyErr_Occurred ()) PyErr_Clear ();


    _moo_register_classes (PyModule_GetDict (_moo_module));

    if (PyErr_Occurred ()) PyErr_Clear ();

    /* HEAPTYPE workaround: tp_methods aren't auto-added to tp_dict for heap types.
     * Re-process tp_methods for all registered types. */
    {
        PyObject *mod_dict = PyModule_GetDict(_moo_module);
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(mod_dict, &pos, &key, &value)) {
            if (PyType_Check(value)) {
                PyTypeObject *tp = (PyTypeObject *)value;
                if (tp->tp_methods && (tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
                    PyMethodDef *ml;
                    for (ml = tp->tp_methods; ml->ml_name != NULL; ml++) {
                        if (!PyDict_GetItemString(tp->tp_dict, ml->ml_name)) {
                            PyObject *descr = PyDescr_NewMethod(tp, ml);
                            if (descr) {
                                PyDict_SetItemString(tp->tp_dict, ml->ml_name, descr);
                                Py_DECREF(descr);
                            } else {
                                PyErr_Clear();
                            }
                        }
                    }
                }
            }
        }
    }

    if (PyErr_Occurred ()) PyErr_Clear ();
    initialized = TRUE;
    return TRUE;
}

PyMODINIT_FUNC
PyInit__moo(void)
{
    if (!_moo_module_init())
        return NULL;
    return PyImport_ImportModule("_moo");
}

static void
init_moo_utils (PyObject *module)
{
    /* Python 3: All strings are Unicode, so these should work as-is */
    PyModule_AddStringConstant (module, "GETTEXT_PACKAGE", GETTEXT_PACKAGE);

    PyModule_AddStringConstant (module, "STOCK_TERMINAL", MOO_STOCK_TERMINAL);
    PyModule_AddStringConstant (module, "STOCK_KEYBOARD", MOO_STOCK_KEYBOARD);
    PyModule_AddStringConstant (module, "STOCK_RESTART", MOO_STOCK_RESTART);
    PyModule_AddStringConstant (module, "STOCK_DOC_DELETED", MOO_STOCK_DOC_DELETED);
    PyModule_AddStringConstant (module, "STOCK_DOC_MODIFIED_ON_DISK", MOO_STOCK_DOC_MODIFIED_ON_DISK);
    PyModule_AddStringConstant (module, "STOCK_DOC_DELETED", MOO_STOCK_DOC_DELETED);
    PyModule_AddStringConstant (module, "STOCK_DOC_MODIFIED", MOO_STOCK_DOC_MODIFIED);
    PyModule_AddStringConstant (module, "STOCK_FILE_SELECTOR", MOO_STOCK_FILE_SELECTOR);
    PyModule_AddStringConstant (module, "STOCK_SAVE_NONE", MOO_STOCK_SAVE_NONE);
    PyModule_AddStringConstant (module, "STOCK_SAVE_SELECTED", MOO_STOCK_SAVE_SELECTED);
    PyModule_AddStringConstant (module, "STOCK_NEW_PROJECT", MOO_STOCK_NEW_PROJECT);
    PyModule_AddStringConstant (module, "STOCK_OPEN_PROJECT", MOO_STOCK_OPEN_PROJECT);
    PyModule_AddStringConstant (module, "STOCK_CLOSE_PROJECT", MOO_STOCK_CLOSE_PROJECT);
    PyModule_AddStringConstant (module, "STOCK_PROJECT_OPTIONS", MOO_STOCK_PROJECT_OPTIONS);
    PyModule_AddStringConstant (module, "STOCK_BUILD", MOO_STOCK_BUILD);
    PyModule_AddStringConstant (module, "STOCK_COMPILE", MOO_STOCK_COMPILE);
    PyModule_AddStringConstant (module, "STOCK_EXECUTE", MOO_STOCK_EXECUTE);
    PyModule_AddStringConstant (module, "STOCK_FIND_IN_FILES", MOO_STOCK_FIND_IN_FILES);
    PyModule_AddStringConstant (module, "STOCK_FIND_FILE", MOO_STOCK_FIND_FILE);
    PyModule_AddStringConstant (module, "STOCK_PLUGINS", MOO_STOCK_PLUGINS);
}
