/*
 *   mooplugin-builtin.c
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
#include <config.h>
#include "mooedit/mooplugin.h"
#include "plugins/mooplugin-builtin.h"
#include "mooutils/mooutils-misc.h"
#ifdef MOO_ENABLE_PYTHON
#include "moopython/moopython-builtin.h"
#endif
#if defined(MOO_BUILD_TERMINAL) || defined(HAVE_VTE)
extern "C" gboolean _moo_terminal_plugin_init(void);
#endif
#if defined(MOO_BUILD_MARKDOWN) || defined(MOO_BUILD_WIKI)
extern "C" gboolean _moo_page_preview_plugin_init(void);
#endif
#ifdef MOO_BUILD_GDB
extern "C" gboolean _moo_gdb_plugin_init(void);
#endif

void
moo_plugin_init (void)
{
#ifdef MOO_ENABLE_PYTHON
    if (!moo_getenv_bool ("MOO_DISABLE_PYTHON"))
        _moo_python_builtin_init ();
#endif
#if defined(MOO_BUILD_TERMINAL) || defined(HAVE_VTE)
    if (!moo_getenv_bool ("MOO_DISABLE_TERMINAL"))
        _moo_terminal_plugin_init ();
#endif
    if (!moo_getenv_bool ("MOO_DISABLE_LUA"))
        _moo_lua_plugin_init ();
    _moo_file_selector_plugin_init ();
    _moo_file_list_plugin_init ();
    _moo_find_plugin_init ();
#ifdef MOO_BUILD_CTAGS
    _moo_ctags_plugin_init ();
#endif
#if defined(MOO_BUILD_MARKDOWN) || defined(MOO_BUILD_WIKI)
    if (!moo_getenv_bool ("MOO_DISABLE_PAGE_PREVIEW"))
        _moo_page_preview_plugin_init ();
#endif
#ifdef MOO_BUILD_GDB
    if (!moo_getenv_bool ("MOO_DISABLE_GDB"))
        _moo_gdb_plugin_init ();
#endif
    moo_plugin_read_dirs ();
    _moo_user_tools_plugin_init ();
}