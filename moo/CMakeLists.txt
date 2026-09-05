# moo/CMakeLists.txt
# Converted from moo/Makefile.am + all Makefile.incl fragments.
# Every source list and custom command is derived directly from the .incl files.

cmake_minimum_required(VERSION 3.16)

set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin "${CMAKE_CURRENT_BINARY_DIR}")
set(_top "${CMAKE_SOURCE_DIR}")

# ────────────────────────────────────────────────────────────────────────────
# Helper: xml2h code-gen  (tools/xml2h.py input output VARNAME)
# ────────────────────────────────────────────────────────────────────────────
function(moo_xml2h input_abs output_rel varname)
    set(_out "${_bin}/${output_rel}")
    get_filename_component(_dir "${_out}" DIRECTORY)
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND "${MOO_PYTHON}" "${_top}/tools/xml2h.py"
                "${input_abs}" "${_out}.tmp" "${varname}"
        COMMAND ${CMAKE_COMMAND} -E rename "${_out}.tmp" "${_out}"
        DEPENDS "${input_abs}" "${_top}/tools/xml2h.py"
        COMMENT "xml2h: ${output_rel}"
        VERBATIM
    )
endfunction()

# ────────────────────────────────────────────────────────────────────────────
# Helper: glade2c code-gen  (tools/glade2c.py glade_abs output_rel)
# ────────────────────────────────────────────────────────────────────────────
function(moo_glade2c glade_abs output_rel)
    set(_out "${_bin}/${output_rel}")
    get_filename_component(_dir "${_out}" DIRECTORY)
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND "${MOO_PYTHON}" "${_top}/tools/glade2c.py"
                "${glade_abs}" > "${_out}.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename "${_out}.tmp" "${_out}"
        DEPENDS "${glade_abs}" "${_top}/tools/glade2c.py"
        COMMENT "glade2c: ${output_rel}"
        VERBATIM
    )
endfunction()

# ════════════════════════════════════════════════════════════════════════════
# marshals.h + marshals.c   (moo/Makefile.am + mooutils/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
add_custom_command(
    OUTPUT  "${_bin}/marshals.h"
    COMMAND "${GLIB_GENMARSHAL}" --prefix=_moo_marshal --header
            "${_src}/marshals.list" > "${_bin}/marshals.h.tmp"
    COMMAND ${CMAKE_COMMAND} -E rename "${_bin}/marshals.h.tmp" "${_bin}/marshals.h"
    DEPENDS "${_src}/marshals.list"
    COMMENT "glib-genmarshal: marshals.h"
    VERBATIM
)
add_custom_command(
    OUTPUT  "${_bin}/marshals.c"
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"marshals.h\"" > "${_bin}/marshals.c.tmp"
    COMMAND "${GLIB_GENMARSHAL}" --prefix=_moo_marshal --body
            "${_src}/marshals.list" >> "${_bin}/marshals.c.tmp"
    COMMAND ${CMAKE_COMMAND} -E rename "${_bin}/marshals.c.tmp" "${_bin}/marshals.c"
    DEPENDS "${_src}/marshals.list"
    COMMENT "glib-genmarshal: marshals.c"
    VERBATIM
)

# ════════════════════════════════════════════════════════════════════════════
# moo-pixbufs.h   (mooutils/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
set(_pixmaps_dir "${_src}/mooutils/pixmaps")
add_custom_command(
    OUTPUT  "${_bin}/moo-pixbufs.h"
    COMMAND "${GDK_PIXBUF_CSOURCE}" --static --build-list
            MOO_HIDE_ICON         "${_pixmaps_dir}/hide.png"
            MOO_CLOSE_ICON        "${_pixmaps_dir}/close.png"
            MOO_STICKY_ICON       "${_pixmaps_dir}/sticky.png"
            MOO_DETACH_ICON       "${_pixmaps_dir}/detach.png"
            MOO_ATTACH_ICON       "${_pixmaps_dir}/attach.png"
            MOO_KEEP_ON_TOP_ICON  "${_pixmaps_dir}/keepontop.png"
            MEDIT_ICON            "${_pixmaps_dir}/medit.png"
            SYMLINK_ARROW         "${_pixmaps_dir}/symlink.png"
            SYMLINK_ARROW_SMALL   "${_pixmaps_dir}/symlink-small.png"
            > "${_bin}/moo-pixbufs.h.tmp"
    COMMAND ${CMAKE_COMMAND} -E rename "${_bin}/moo-pixbufs.h.tmp" "${_bin}/moo-pixbufs.h"
    DEPENDS
        "${_pixmaps_dir}/hide.png" "${_pixmaps_dir}/close.png"
        "${_pixmaps_dir}/sticky.png" "${_pixmaps_dir}/detach.png"
        "${_pixmaps_dir}/attach.png" "${_pixmaps_dir}/keepontop.png"
        "${_pixmaps_dir}/medit.png" "${_pixmaps_dir}/symlink.png"
        "${_pixmaps_dir}/symlink-small.png"
    COMMENT "gdk-pixbuf-csource: moo-pixbufs.h"
    VERBATIM
)

# ════════════════════════════════════════════════════════════════════════════
# mooapp-credits.h   (mooapp/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
moo_xml2h("${_top}/THANKS" "mooapp-credits.h" "MOO_APP_CREDITS")

# ════════════════════════════════════════════════════════════════════════════
# Glade → *-gxml.h headers
# ════════════════════════════════════════════════════════════════════════════

# mooapp
moo_glade2c("${_src}/mooapp/glade/mooappabout-dialog.glade"   "mooapp/mooappabout-dialog-gxml.h")
moo_glade2c("${_src}/mooapp/glade/mooappabout-license.glade"  "mooapp/mooappabout-license-gxml.h")
moo_glade2c("${_src}/mooapp/glade/mooappabout-credits.glade"  "mooapp/mooappabout-credits-gxml.h")

# mooutils
moo_glade2c("${_src}/mooutils/glade/mooaccelprefs.glade"    "mooutils/mooaccelprefs-gxml.h")
moo_glade2c("${_src}/mooutils/glade/mooaccelbutton.glade"   "mooutils/mooaccelbutton-gxml.h")
moo_glade2c("${_src}/mooutils/glade/moologwindow.glade"     "mooutils/moologwindow-gxml.h")

# moofileview
moo_glade2c("${_src}/moofileview/glade/moofileview-drop.glade"     "moofileview/moofileview-drop-gxml.h")
moo_glade2c("${_src}/moofileview/glade/moofileprops.glade"         "moofileview/moofileprops-gxml.h")
moo_glade2c("${_src}/moofileview/glade/moocreatefolder.glade"      "moofileview/moocreatefolder-gxml.h")
moo_glade2c("${_src}/moofileview/glade/moobookmark-editor.glade"   "moofileview/moobookmark-editor-gxml.h")

# mooedit
moo_glade2c("${_src}/mooedit/glade/moopluginprefs.glade"         "mooedit/moopluginprefs-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditprefs-view.glade"      "mooedit/mooeditprefs-view-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditprefs-file.glade"      "mooedit/mooeditprefs-file-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditprefs-filters.glade"   "mooedit/mooeditprefs-filters-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditprefs-general.glade"   "mooedit/mooeditprefs-general-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditprefs-langs.glade"     "mooedit/mooeditprefs-langs-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditprogress.glade"        "mooedit/mooeditprogress-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooeditsavemult.glade"        "mooedit/mooeditsavemult-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mootryencoding.glade"         "mooedit/mootryencoding-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooprint.glade"               "mooedit/mooprint-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mootextfind.glade"            "mooedit/mootextfind-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mootextfind-prompt.glade"     "mooedit/mootextfind-prompt-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mootextgotoline.glade"        "mooedit/mootextgotoline-gxml.h")
moo_glade2c("${_src}/mooedit/glade/mooquicksearch.glade"         "mooedit/mooquicksearch-gxml.h")
moo_glade2c("${_src}/mooedit/glade/moostatusbar.glade"           "mooedit/moostatusbar-gxml.h")

# plugins
moo_glade2c("${_src}/plugins/glade/moofileselector-prefs.glade" "plugins/moofileselector-prefs-gxml.h")
moo_glade2c("${_src}/plugins/glade/moofileselector.glade"       "plugins/moofileselector-gxml.h")
moo_glade2c("${_src}/plugins/glade/moofind.glade"               "plugins/moofind-gxml.h")
moo_glade2c("${_src}/plugins/glade/moogrep.glade"               "plugins/moogrep-gxml.h")

# plugins/usertools
moo_glade2c("${_src}/plugins/usertools/glade/mooedittools-exe.glade"    "plugins/usertools/mooedittools-exe-gxml.h")
moo_glade2c("${_src}/plugins/usertools/glade/mooedittools-script.glade" "plugins/usertools/mooedittools-script-gxml.h")
moo_glade2c("${_src}/plugins/usertools/glade/moousertools.glade"        "plugins/usertools/moousertools-gxml.h")

# ════════════════════════════════════════════════════════════════════════════
# XML / UI headers (xml2h)
# ════════════════════════════════════════════════════════════════════════════
moo_xml2h("${_src}/moofileview/moofileview.xml" "moofileview/moofileview-ui.h" "moofileview_ui_xml")
moo_xml2h("${_src}/mooedit/medit.xml"           "mooedit/medit-ui.h"           "medit_ui_xml")
moo_xml2h("${_src}/mooedit/mooedit.xml"         "mooedit/mooedit-ui.h"         "mooedit_ui_xml")

# ════════════════════════════════════════════════════════════════════════════
# Lua init headers  (moolua/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
moo_xml2h("${_src}/moolua/lua-default-init.lua" "moolua/lua-default-init.h" "LUA_DEFAULT_INIT")
moo_xml2h("${_src}/moolua/lua-module-init.lua"  "moolua/lua-module-init.h"  "LUA_MODULE_INIT")
moo_xml2h("${_src}/moolua/lua-plugin-init.lua"  "moolua/lua-plugin-init.h"  "LUA_PLUGIN_INIT")

# ════════════════════════════════════════════════════════════════════════════
# plugins/usertools generated files
# ════════════════════════════════════════════════════════════════════════════
moo_xml2h("${_src}/plugins/usertools/lua-tool-setup.lua"
          "plugins/usertools/lua-tool-setup.h" "LUA_TOOL_SETUP_LUA")

if(MOO_ENABLE_PYTHON)
    moo_xml2h("${_src}/plugins/usertools/python-tool-setup.py"
              "plugins/usertools/python-tool-setup.h" "PYTHON_TOOL_SETUP_PY")
endif()

# plugins/usertools/menu.xml and context.xml from templates
foreach(_tmpl menu context)
    set(_out "${_bin}/plugins/usertools/${_tmpl}.xml")
    set(_genplatxml_args "")
    if(MOO_OS_WIN32)
        list(APPEND _genplatxml_args --enable=win32)
    else()
        list(APPEND _genplatxml_args --enable=unix)
    endif()
    if(MOO_ENABLE_PYTHON)
        list(APPEND _genplatxml_args --enable=python)
    endif()
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/plugins/usertools"
        COMMAND "${MOO_PYTHON}"
                "${_src}/plugins/usertools/genplatxml.py"
                ${_genplatxml_args}
                "${_src}/plugins/usertools/${_tmpl}-tmpl.xml"
                > "${_out}.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename "${_out}.tmp" "${_out}"
        DEPENDS "${_src}/plugins/usertools/${_tmpl}-tmpl.xml"
                "${_src}/plugins/usertools/genplatxml.py"
        COMMENT "genplatxml: plugins/usertools/${_tmpl}.xml"
        VERBATIM
    )
endforeach()

# ════════════════════════════════════════════════════════════════════════════
# moopython/medit-python-init.h   (moopython/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
moo_xml2h("${_src}/moopython/medit-python-init.py"
          "moopython/medit-python-init.h" "MEDIT_PYTHON_INIT_PY")

# ════════════════════════════════════════════════════════════════════════════
# moopython/pygtk  code-gen  (moopython/pygtk/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
if(MOO_ENABLE_PYTHON)
    # moo-mod.h  (xml2h of moo.py)
    moo_xml2h("${_src}/moopython/pygtk/moo.py" "moopython/pygtk/moo-mod.h" "MOO_PY")

    # moo.defs  (gendefs.py)
    add_custom_command(
        OUTPUT  "${_bin}/moopython/pygtk/moo.defs"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/moopython/pygtk"
        COMMAND "${MOO_PYTHON}" "${_top}/api/gendefs.py"
                "${_top}/api/moo.xml"
                > "${_bin}/moopython/pygtk/moo.defs.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename
                "${_bin}/moopython/pygtk/moo.defs.tmp"
                "${_bin}/moopython/pygtk/moo.defs"
        DEPENDS "${_top}/api/gendefs.py"
                "${_top}/api/moo.xml"
                "${_top}/api/mpi/__init__.py"
                "${_top}/api/mpi/module.py"
                "${_top}/api/mpi/defswriter.py"
        COMMENT "gendefs: moopython/pygtk/moo.defs"
        VERBATIM
    )

    # moo-mod.cpp  (codegen)
    set(_codegen_platform "")
    if(MOO_OS_WIN32)
        set(_codegen_platform --platform win32)
    endif()
    add_custom_command(
        OUTPUT  "${_bin}/moopython/pygtk/moo-mod.cpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/moopython/pygtk"
        COMMAND "${MOO_PYTHON}"
                "${_src}/moopython/codegen/codegen.py"
                ${_codegen_platform}
                --codebefore "${_src}/moopython/pygtk/codebefore.c"
                --codeafter  "${_src}/moopython/pygtk/codeafter.c"
                --prefix _moo
                --load-types "${_src}/moopython/codegen/argtypes_m.py"
                --register "${PYGOBJECT_DEFS_DIR}/gio-types.defs"
                --register "${PYGTK_DEFS_DIR}/gtk-types.defs"
                --register "${PYGTK_DEFS_DIR}/gdk-types.defs"
                --override "${_src}/moopython/pygtk/moo.override"
                "${_bin}/moopython/pygtk/moo.defs"
                > "${_bin}/moopython/pygtk/moo-mod.cpp.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename
                "${_bin}/moopython/pygtk/moo-mod.cpp.tmp"
                "${_bin}/moopython/pygtk/moo-mod.cpp"
        DEPENDS "${_bin}/moopython/pygtk/moo.defs"
                "${_src}/moopython/pygtk/moo.override"
                "${_src}/moopython/pygtk/mooutils.override"
                "${_src}/moopython/pygtk/codebefore.c"
                "${_src}/moopython/pygtk/codeafter.c"
                "${_src}/moopython/codegen/codegen.py"
                "${_src}/moopython/codegen/argtypes.py"
                "${_src}/moopython/codegen/argtypes_m.py"
                "${_src}/moopython/codegen/reversewrapper.py"
        COMMENT "codegen: moopython/pygtk/moo-mod.cpp"
        VERBATIM
    )

    # moopython/plugins/*.ini from *.ini.in  (configure substitution)
    foreach(_ini terminal python)
        configure_file(
            "${_src}/moopython/plugins/${_ini}.ini.in"
            "${_bin}/moopython/plugins/${_ini}.ini"
            @ONLY
        )
    endforeach()
endif()

# ════════════════════════════════════════════════════════════════════════════
# Dev-mode: glib-mkenums  (mooedit + mooutils enum types)
# ════════════════════════════════════════════════════════════════════════════
if(MOO_DEV_MODE)
    # mooedit enum types
    set(_moo_edit_enum_headers "${_src}/mooedit/mooedit-enums.h")
    foreach(_kind h c)
        add_custom_command(
            OUTPUT  "${_bin}/mooedit/mooedit-enum-types.${_kind}.stamp"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/mooedit"
            COMMAND "${GLIB_MKENUMS}"
                    --template "${_src}/mooedit/mooedit-enum-types.${_kind}.tmpl"
                    ${_moo_edit_enum_headers}
                    > "${_bin}/mooedit/mooedit-enum-types.${_kind}.tmp"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_bin}/mooedit/mooedit-enum-types.${_kind}.tmp"
                    "${_src}/mooedit/mooedit-enum-types.${_kind}"
            COMMAND ${CMAKE_COMMAND} -E touch
                    "${_bin}/mooedit/mooedit-enum-types.${_kind}.stamp"
            DEPENDS ${_moo_edit_enum_headers}
                    "${_src}/mooedit/mooedit-enum-types.${_kind}.tmpl"
            COMMENT "glib-mkenums: mooedit-enum-types.${_kind}"
            VERBATIM
        )
    endforeach()

    # mooutils enum types
    set(_moo_utils_enum_headers
        "${_src}/mooutils/moodialogs.h"
        "${_src}/mooutils/moofiledialog.h"
        "${_src}/mooutils/moouixml.h"
        "${_src}/mooutils/moowindow.h"
    )
    foreach(_kind h c)
        add_custom_command(
            OUTPUT  "${_bin}/mooutils/mooutils-enums.${_kind}.stamp"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/mooutils"
            COMMAND "${GLIB_MKENUMS}"
                    --template "${_src}/mooutils/mooutils-enums.tmpl.${_kind}"
                    ${_moo_utils_enum_headers}
                    > "${_bin}/mooutils/mooutils-enums.${_kind}.tmp"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_bin}/mooutils/mooutils-enums.${_kind}.tmp"
                    "${_src}/mooutils/mooutils-enums.${_kind}"
            COMMAND ${CMAKE_COMMAND} -E touch
                    "${_bin}/mooutils/mooutils-enums.${_kind}.stamp"
            DEPENDS ${_moo_utils_enum_headers}
                    "${_src}/mooutils/mooutils-enums.tmpl.${_kind}"
            COMMENT "glib-mkenums: mooutils-enums.${_kind}"
            VERBATIM
        )
    endforeach()

    # Dev-mode: moolua generated Lua API C++ files  (moolua/Makefile.incl)
    set(_genlua_files
        "${_top}/api/genlua.py"
        "${_top}/api/mpi/__init__.py"
        "${_top}/api/mpi/module.py"
        "${_top}/api/mpi/luawriter.py"
    )
    add_custom_command(
        OUTPUT  "${_src}/moolua/moo-lua-api.cpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/moolua"
        COMMAND "${MOO_PYTHON}" "${_top}/api/genlua.py"
                --include-header moo-lua-api.h
                --import "${_top}/api/gtk.xml"
                "${_top}/api/moo.xml"
                > "${_bin}/moolua/moo-lua-api.cpp.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename
                "${_bin}/moolua/moo-lua-api.cpp.tmp"
                "${_src}/moolua/moo-lua-api.cpp"
        DEPENDS ${_genlua_files} "${_top}/api/moo.xml" "${_top}/api/gtk.xml"
        COMMENT "genlua: moolua/moo-lua-api.cpp"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  "${_src}/moolua/gtk-lua-api.cpp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}/moolua"
        COMMAND "${MOO_PYTHON}" "${_top}/api/genlua.py"
                --include-header gtk-lua-api.h
                "${_top}/api/gtk.xml"
                > "${_bin}/moolua/gtk-lua-api.cpp.tmp"
        COMMAND ${CMAKE_COMMAND} -E rename
                "${_bin}/moolua/gtk-lua-api.cpp.tmp"
                "${_src}/moolua/gtk-lua-api.cpp"
        DEPENDS ${_genlua_files} "${_top}/api/gtk.xml"
        COMMENT "genlua: moolua/gtk-lua-api.cpp"
        VERBATIM
    )
endif()

# ════════════════════════════════════════════════════════════════════════════
# Windows .rc → .res   (medit-app/Makefile.incl, medit-module/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
set(_medit_res "")
set(_moo_module_res "")
if(MOO_OS_WIN32 AND WINDRES)
    if(MOO_BUILD_APP)
        # The .rc.in template was written for autotools and references
        # @top_srcdir@; provide that as a CMake variable so configure_file's
        # @ONLY substitution emits a real path instead of leaving it empty
        # (windres would then fail to open `/moo/mooutils/pixmaps/medit.ico`).
        set(top_srcdir "${_top}")
        configure_file("${_src}/medit-app/medit.rc.in"
                       "${_bin}/medit-app/medit.rc" @ONLY)
        add_custom_command(
            OUTPUT  "${_bin}/medit-app/medit.res"
            COMMAND "${WINDRES}" -i "${_bin}/medit-app/medit.rc"
                    --input-format=rc -o "${_bin}/medit-app/medit.res" -O coff
            DEPENDS "${_bin}/medit-app/medit.rc"
            COMMENT "windres: medit-app/medit.res"
            VERBATIM
        )
        set(_medit_res "${_bin}/medit-app/medit.res")
    endif()

    # medit-module/moo.rc.in only ships in builds that include the standalone
    # Python module.  Skip it otherwise (the file isn't in the source tree).
    if(MOO_BUILD_MODULE AND MOO_ENABLE_PYTHON AND
       EXISTS "${_src}/medit-module/moo.rc.in")
        configure_file("${_src}/medit-module/moo.rc.in"
                       "${_bin}/medit-module/moo.rc" @ONLY)
        add_custom_command(
            OUTPUT  "${_bin}/medit-module/moo.res"
            COMMAND "${WINDRES}" -i "${_bin}/medit-module/moo.rc"
                    --input-format=rc -o "${_bin}/medit-module/moo.res" -O coff
            DEPENDS "${_bin}/medit-module/moo.rc"
            COMMENT "windres: medit-module/moo.res"
            VERBATIM
        )
        set(_moo_module_res "${_bin}/medit-module/moo.res")
    endif()
endif()

# ════════════════════════════════════════════════════════════════════════════
# medit-app/medit.desktop  (Unix only, intltool-merged)
# ════════════════════════════════════════════════════════════════════════════
if(MOO_OS_UNIX AND MOO_BUILD_APP)
    moo_intltool_merge(
        INPUT  "${_src}/medit-app/medit.desktop.in"
        OUTPUT "${_bin}/medit-app/medit.desktop"
    )
    # Make the .desktop file part of the default build so install() / cpack
    # find it.  add_custom_command(OUTPUT ...) only fires when a target lists
    # the output as a dependency; without this ALL-target it is never built.
    add_custom_target(medit-desktop ALL
        DEPENDS "${_bin}/medit-app/medit.desktop"
    )
    install(
        FILES       "${_bin}/medit-app/medit.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )

    # AppStream metainfo, so medit shows up in GNOME Software / KDE Discover /
    # Flathub.  Not translated through intltool: the file has no _-prefixed
    # keys, and intltool is on its way out anyway.
    string(TIMESTAMP _moo_release_date "%Y-%m-%d" UTC)
    set(MOO_RELEASE_DATE "${_moo_release_date}")
    configure_file(
        "${_top}/data/medit.metainfo.xml.in"
        "${_bin}/data/io.github.fangq.medit.metainfo.xml"
        @ONLY
    )
    install(
        FILES       "${_bin}/data/io.github.fangq.medit.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
    )
endif()

# ════════════════════════════════════════════════════════════════════════════
# Icon
# ════════════════════════════════════════════════════════════════════════════
install(
    FILES       "${_src}/mooutils/pixmaps/medit.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps"
)

# ════════════════════════════════════════════════════════════════════════════
# Language specs and colour schemes  (mooedit/langs/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
install(
    DIRECTORY   "${_src}/mooedit/langs/"
    DESTINATION "${MOO_TEXT_LANG_FILES_DIR}"
    FILES_MATCHING
        PATTERN "*.lang"
        PATTERN "*.xml"
        PATTERN "*.rng"
        PATTERN "check.sh"
)

# ════════════════════════════════════════════════════════════════════════════
# Lua _moo/*.lua data files  (moolua/lua/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
install(
    DIRECTORY   "${_src}/moolua/lua/_moo/"
    DESTINATION "${MOO_DATA_DIR}/lua/_moo"
    FILES_MATCHING PATTERN "*.lua"
)

# ════════════════════════════════════════════════════════════════════════════
# Python plugin data files  (moopython/plugins/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
if(MOO_ENABLE_PYTHON)
    install(
        FILES
            "${_bin}/moopython/plugins/terminal.ini"
            "${_bin}/moopython/plugins/python.ini"
            "${_src}/moopython/plugins/terminal.py"
            "${_src}/moopython/plugins/python.py"
        DESTINATION "${MOO_PYTHON_PLUGIN_DIR}"
    )
    install(
        FILES
            "${_src}/moopython/plugins/lib/pyconsole.py"
            "${_src}/moopython/plugins/lib/insert_date_and_time.py"
        DESTINATION "${MOO_PYTHON_LIB_DIR}"
    )
    install(
        FILES
            "${_src}/moopython/plugins/medit/__init__.py"
            "${_src}/moopython/plugins/medit/runpython.py"
        DESTINATION "${MOO_PYTHON_LIB_DIR}/medit"
    )
    install(
        FILES "${_src}/moopython/pygtk/moo.py"
        DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/python${PYTHON_VERSION}/site-packages"
    )
endif()

# ════════════════════════════════════════════════════════════════════════════
# plugins/usertools data files
# ════════════════════════════════════════════════════════════════════════════
install(
    FILES
        "${_src}/plugins/usertools/filters.xml"
        "${_bin}/plugins/usertools/menu.xml"
        "${_bin}/plugins/usertools/context.xml"
    DESTINATION "${MOO_DATA_DIR}"
)

# ════════════════════════════════════════════════════════════════════════════
# Collect all generated built sources
# ════════════════════════════════════════════════════════════════════════════
# ── Generated headers (not compiled directly; #included by .c/.cpp files) ────
# Must exist before compilation. Tracked via moo_codegen target below.
set(_generated_headers
    "${_bin}/marshals.h"
    "${_bin}/moo-pixbufs.h"
    "${_bin}/mooapp-credits.h"
    "${_bin}/mooapp/mooappabout-dialog-gxml.h"
    "${_bin}/mooapp/mooappabout-license-gxml.h"
    "${_bin}/mooapp/mooappabout-credits-gxml.h"
    "${_bin}/mooutils/mooaccelprefs-gxml.h"
    "${_bin}/mooutils/mooaccelbutton-gxml.h"
    "${_bin}/mooutils/moologwindow-gxml.h"
    "${_bin}/moofileview/moofileview-drop-gxml.h"
    "${_bin}/moofileview/moofileprops-gxml.h"
    "${_bin}/moofileview/moocreatefolder-gxml.h"
    "${_bin}/moofileview/moobookmark-editor-gxml.h"
    "${_bin}/moofileview/moofileview-ui.h"
    "${_bin}/mooedit/moopluginprefs-gxml.h"
    "${_bin}/mooedit/mooeditprefs-view-gxml.h"
    "${_bin}/mooedit/mooeditprefs-file-gxml.h"
    "${_bin}/mooedit/mooeditprefs-filters-gxml.h"
    "${_bin}/mooedit/mooeditprefs-general-gxml.h"
    "${_bin}/mooedit/mooeditprefs-langs-gxml.h"
    "${_bin}/mooedit/mooeditprogress-gxml.h"
    "${_bin}/mooedit/mooeditsavemult-gxml.h"
    "${_bin}/mooedit/mootryencoding-gxml.h"
    "${_bin}/mooedit/mooprint-gxml.h"
    "${_bin}/mooedit/mootextfind-gxml.h"
    "${_bin}/mooedit/mootextfind-prompt-gxml.h"
    "${_bin}/mooedit/mootextgotoline-gxml.h"
    "${_bin}/mooedit/mooquicksearch-gxml.h"
    "${_bin}/mooedit/moostatusbar-gxml.h"
    "${_bin}/mooedit/medit-ui.h"
    "${_bin}/mooedit/mooedit-ui.h"
    "${_bin}/moolua/lua-default-init.h"
    "${_bin}/moolua/lua-module-init.h"
    "${_bin}/moolua/lua-plugin-init.h"
    "${_bin}/moopython/medit-python-init.h"
    "${_bin}/plugins/moofileselector-prefs-gxml.h"
    "${_bin}/plugins/moofileselector-gxml.h"
    "${_bin}/plugins/moofind-gxml.h"
    "${_bin}/plugins/moogrep-gxml.h"
    "${_bin}/plugins/usertools/mooedittools-exe-gxml.h"
    "${_bin}/plugins/usertools/mooedittools-script-gxml.h"
    "${_bin}/plugins/usertools/moousertools-gxml.h"
    "${_bin}/plugins/usertools/lua-tool-setup.h"
    "${_bin}/plugins/usertools/menu.xml"
    "${_bin}/plugins/usertools/context.xml"
)
if(MOO_ENABLE_PYTHON)
    list(APPEND _generated_headers
        "${_bin}/moopython/pygtk/moo-mod.h"
        "${_bin}/moopython/pygtk/moo.defs"
        "${_bin}/plugins/usertools/python-tool-setup.h"
    )
endif()

# ── Compilable generated sources (.c/.cpp that are actually compiled) ─────────
set(_built_compilable_sources
    "${_bin}/marshals.c"
)
if(MOO_ENABLE_PYTHON)
    list(APPEND _built_compilable_sources
        "${_bin}/moopython/pygtk/moo-mod.cpp"
    )
endif()

# ── Windows .res files (passed to linker) ─────────────────────────────────────
set(_medit_res_src "")
set(_moo_module_res_src "")
if(MOO_OS_WIN32 AND WINDRES)
    set(_medit_res_src      "${_medit_res}")
    set(_moo_module_res_src "${_moo_module_res}")
endif()

# ── Custom target that drives all header code generation ──────────────────────
# medit and _moo add_dependencies() on this so headers exist before compiling.
add_custom_target(moo_codegen ALL
    DEPENDS ${_generated_headers}
)

# ════════════════════════════════════════════════════════════════════════════
# Explicit source lists from all Makefile.incl files
# ════════════════════════════════════════════════════════════════════════════

# ── mooglib ──────────────────────────────────────────────────────────────────
# moo-glib.cpp builds on every platform (its Win32 specifics are guarded
# inside the file with #ifdef __WIN32__).  The autotools build adds it
# unconditionally; mirror that here.  There's no separately-built
# libmooglib library — the source is compiled into medit.exe directly.
set(_mooglib_sources
    mooglib/moo-glib.h
    mooglib/moo-stat.h
    mooglib/moo-time.h
    mooglib/moo-glib.cpp
)

# ── xdgmime ──────────────────────────────────────────────────────────────────
set(_xdgmime_sources
    xdgmime/xdgmimealias.c  xdgmime/xdgmimealias.h
    xdgmime/xdgmime.c       xdgmime/xdgmime.h
    xdgmime/xdgmimecache.c  xdgmime/xdgmimecache.h
    xdgmime/xdgmimeglob.c   xdgmime/xdgmimeglob.h
    xdgmime/xdgmimeint.h
    xdgmime/xdgmimeicon.c   xdgmime/xdgmimeicon.h
    xdgmime/xdgmimemagic.c  xdgmime/xdgmimemagic.h
    xdgmime/xdgmimeparent.c xdgmime/xdgmimeparent.h
)

# ── gtksourceview ─────────────────────────────────────────────────────────────
set(_gtksourceview_sources
    gtksourceview/gtksourcecontextengine.c  gtksourceview/gtksourcecontextengine.h
    gtksourceview/gtksourceengine.c         gtksourceview/gtksourceengine.h
    gtksourceview/gtksourceiter.c           gtksourceview/gtksourceiter.h
    gtksourceview/gtksourcelanguage-parser-1.c
    gtksourceview/gtksourcelanguage-parser-2.c
    gtksourceview/gtksourcelanguage-private.h
    gtksourceview/gtksourcelanguage.c       gtksourceview/gtksourcelanguage.h
    gtksourceview/gtksourcelanguagemanager.c gtksourceview/gtksourcelanguagemanager.h
    gtksourceview/gtksourcestyle-private.h
    gtksourceview/gtksourcestyle.c          gtksourceview/gtksourcestyle.h
    gtksourceview/gtksourcestylescheme.c    gtksourceview/gtksourcestylescheme.h
    gtksourceview/gtksourcestyleschememanager.c gtksourceview/gtksourcestyleschememanager.h
    gtksourceview/gtksourceview-utils.c     gtksourceview/gtksourceview-utils.h
    gtksourceview/gtktextregion.c           gtksourceview/gtktextregion.h
    gtksourceview/gtksourceview-i18n.h
    gtksourceview/gtksourceview-marshal.h
    gtksourceview/gtksourceview-api.h
    gtksourceview/gtksourcebuffer.h
    gtksourceview/gtksourceview.h
)

# ── gtksourceview: define DEBUG(x) via forced-include ────────────────────────
# gtksourceview uses  DEBUG(expr)  as a function-like logging macro.
# CMake drops function-like macros from -D flags, so we write a tiny header
# and force-include it only for gtksourceview .c files with -include.
set(_gsv_debug_h "${_bin}/gtksourceview-debug.h")
file(WRITE "${_gsv_debug_h}"
"/* Auto-generated — do not edit */
"
"/* The gtksourceview .c sources define GSV_DEBUG themselves; this header
   only neutralises a stray DEBUG macro that may leak in from -D flags.
   Don't define GSV_DEBUG here — doing so would conflict with the per-
   source definitions and emit a 'redefined' warning. */
"
"#ifndef GTKSOURCEVIEW_DEBUG_H
"
"#define GTKSOURCEVIEW_DEBUG_H
"
"#  ifdef DEBUG
"
"#    undef DEBUG
"
"#  endif
"
"#endif
"
)

set(_gsv_c_files "")
foreach(_rel ${_gtksourceview_sources})
    if(_rel MATCHES "\\.c$")
        list(APPEND _gsv_c_files "${_src}/${_rel}")
    endif()
endforeach()
if(_gsv_c_files)
    set_source_files_properties(${_gsv_c_files} PROPERTIES
        COMPILE_OPTIONS "-include;${_gsv_debug_h}"
    )
endif()

# moouixml.c uses a discriminated-union pattern: node_new() allocates
# sizeof(MooUiNode) (40 B) for CONTAINER nodes and sizeof(MooUiItemNode)
# (larger) for ITEM nodes; node_free() switches on node->type before
# casting to (Item*).  gcc 13's interprocedural -Warray-bounds= inlines
# item_free() into parse_widget/parse_placeholder, loses track of that
# runtime switch, and falsely flags every g_free(item->...) as
# out-of-bounds.  Suppress for this TU only.
set_source_files_properties("${_src}/mooutils/moouixml.c" PROPERTIES
    COMPILE_OPTIONS "-Wno-array-bounds"
)

# ── eggsmclient ───────────────────────────────────────────────────────────────
set(_eggsmclient_sources
    eggsmclient/eggsmclient.c
    eggsmclient/eggsmclient.h
    eggsmclient/eggsmclient-mangle.h
    eggsmclient/eggsmclient-private.h
)
if(MOO_OS_WIN32)
    list(APPEND _eggsmclient_sources eggsmclient/eggsmclient-win32.c)
elseif(MOO_OS_DARWIN AND GDK_QUARTZ)
    # Native Quartz backend: provide the egg_sm_client_osx_new() symbol that
    # eggsmclient.c calls when GDK_WINDOWING_QUARTZ is defined.
    list(APPEND _eggsmclient_sources eggsmclient/eggsmclient-osx.c)
elseif(MOO_OS_DARWIN AND GDK_X11)
    # macOS + XQuartz: use the same XSMP backend as Linux.
    list(APPEND _eggsmclient_sources
        eggsmclient/eggsmclient-xsmp.c
        eggsmclient/eggdesktopfile.h
        eggsmclient/eggdesktopfile.c
    )
else()
    # Linux / other Unix — XSMP backend.
    # Add -DEGG_SM_CLIENT_BACKEND_XSMP via compile definitions below.
    list(APPEND _eggsmclient_sources
        eggsmclient/eggsmclient-xsmp.c
        eggsmclient/eggdesktopfile.h
        eggsmclient/eggdesktopfile.c
    )
endif()

# ── moocpp ───────────────────────────────────────────────────────────────────
set(_moocpp_sources
    moocpp/fileutils.h  moocpp/fileutils.cpp
    moocpp/gobjptr.h
    moocpp/gstr.h       moocpp/gstr.cpp
    moocpp/moocpp.h
    moocpp/regex.h      moocpp/regex.cpp
    moocpp/util.h
)

# ── mooutils ──────────────────────────────────────────────────────────────────
set(_mooutils_sources
    mooutils/moodialogs.h   mooutils/moofiledialog.h
    mooutils/moouixml.h     mooutils/moowindow.h
    mooutils/mooarray.h
    mooutils/mooutils-thread.cpp    mooutils/mooutils-thread.h
    mooutils/moohistorymgr.c        mooutils/moohistorymgr.h
    mooutils/moo-environ.h
    mooutils/mooaccel.cpp           mooutils/mooaccel.h
    mooutils/mooaccelbutton.c       mooutils/mooaccelbutton.h
    mooutils/mooaccelprefs.c        mooutils/mooaccelprefs.h
    mooutils/mooaction-private.h
    mooutils/mooaction.c            mooutils/mooaction.h
    mooutils/mooactionbase-private.h
    mooutils/mooactionbase.c        mooutils/mooactionbase.h
    mooutils/mooactioncollection.c  mooutils/mooactioncollection.h
    mooutils/mooactionfactory.c     mooutils/mooactionfactory.h
    mooutils/mooactiongroup.c       mooutils/mooactiongroup.h
    mooutils/mooapp-ipc.c           mooutils/mooapp-ipc.h
    mooutils/mooappinput-common.c   mooutils/mooappinput.h
    mooutils/mooappinput-priv.h
    mooutils/mooatom.h
    mooutils/moobigpaned.c          mooutils/moobigpaned.h
    mooutils/mooclosure.c           mooutils/mooclosure.h
    mooutils/moocombo.c             mooutils/moocombo.h
    mooutils/moocompat.h
    mooutils/moodialogs.c
    mooutils/mooeditops.c           mooutils/mooeditops.h
    mooutils/mooencodings-data.h
    mooutils/mooencodings.c         mooutils/mooencodings.h
    mooutils/mooentry.cpp           mooutils/mooentry.h
    mooutils/moofiledialog.c
    mooutils/moofileicon.c          mooutils/moofileicon.h
    mooutils/moofilewatch.c         mooutils/moofilewatch.h
    mooutils/moofilewriter.cpp      mooutils/moofilewriter.h
    mooutils/moofilewriter-private.h
    mooutils/moofiltermgr.c         mooutils/moofiltermgr.h
    mooutils/moofontsel.c           mooutils/moofontsel.h
    mooutils/mooglade.c             mooutils/mooglade.h
    mooutils/moohelp.c              mooutils/moohelp.h
    mooutils/moohistorycombo.c      mooutils/moohistorycombo.h
    mooutils/moohistorylist.c       mooutils/moohistorylist.h
    mooutils/mooi18n.cpp            mooutils/mooi18n.h
    mooutils/moolist.h
    mooutils/moomarkup.c            mooutils/moomarkup.h
    mooutils/moomenu.c              mooutils/moomenu.h
    mooutils/moomenuaction.c        mooutils/moomenuaction.h
    mooutils/moomenumgr.c           mooutils/moomenumgr.h
    mooutils/moomenutoolbutton.c    mooutils/moomenutoolbutton.h
    mooutils/moo-mime.c             mooutils/moo-mime.h
    mooutils/moonotebook.c          mooutils/moonotebook.h
    mooutils/mooonce.h
    mooutils/moopane.c              mooutils/moopane.h
    mooutils/moopaned.c             mooutils/moopaned.h
    mooutils/mooprefs.c             mooutils/mooprefs.h
    mooutils/mooprefsdialog.c       mooutils/mooprefsdialog.h
    mooutils/mooprefspage.c         mooutils/mooprefspage.h
    mooutils/moospawn.c             mooutils/moospawn.h
    mooutils/moostock.c             mooutils/moostock.h
    mooutils/mootype-macros.h
    mooutils/moouixml.c
    mooutils/mooundo.c              mooutils/mooundo.h
    mooutils/mooutils.h             mooutils/mooutils-cpp.h
    mooutils/mooutils-debug.h
    mooutils/mooutils-enums.c       mooutils/mooutils-enums.h
    mooutils/mooutils-file.c        mooutils/mooutils-file.h
    mooutils/mooutils-fs.cpp        mooutils/mooutils-fs.h
    mooutils/mooutils-gobject-private.h
    mooutils/mooutils-gobject.cpp   mooutils/mooutils-gobject.h
    mooutils/mooutils-gpp.h         mooutils/mooutils-macros.h
    mooutils/mooutils-mem.h         mooutils/mooutils-messages.h
    mooutils/mooutils-misc.cpp      mooutils/mooutils-misc.h
    mooutils/mooutils-script.c      mooutils/mooutils-script.h
    mooutils/mooutils-tests.h
    mooutils/mooutils-treeview.c    mooutils/mooutils-treeview.h
    mooutils/moowindow.c
    mooutils/stock-file-24.h
    mooutils/stock-file-selector-24.h
    mooutils/stock-terminal-24.h
    # test utils (always compiled in)
    mooutils/moo-test-utils.cpp     mooutils/moo-test-utils.h
    mooutils/moo-test-macros.h
)
if(MOO_OS_WIN32)
    list(APPEND _mooutils_sources
        mooutils/mooutils-win32.cpp
        mooutils/mooappinput-win32.cpp
    )
else()
    list(APPEND _mooutils_sources mooutils/mooappinput-unix.c)
endif()

# ── moofileview ───────────────────────────────────────────────────────────────
set(_moofileview_sources
    moofileview/moobookmarkmgr.c    moofileview/moobookmarkmgr.h
    moofileview/moobookmarkview.c   moofileview/moobookmarkview.h
    moofileview/moofile.c           moofileview/moofile.h
    moofileview/moofile-private.h
    moofileview/moofileentry.c      moofileview/moofileentry.h
    moofileview/moofilesystem.c     moofileview/moofilesystem.h
    moofileview/moofileview.c       moofileview/moofileview.h
    moofileview/moofileview-accels.h
    moofileview/moofileview-aux.h
    moofileview/moofileview-dialogs.c moofileview/moofileview-dialogs.h
    moofileview/moofileview-impl.h
    moofileview/moofileview-private.h
    moofileview/moofileview-tools.c moofileview/moofileview-tools.h
    moofileview/moofolder-private.h
    moofileview/moofolder.c         moofileview/moofolder.h
    moofileview/moofoldermodel.c    moofileview/moofoldermodel.h
    moofileview/moofoldermodel-private.h
    moofileview/mooiconview.c       moofileview/mooiconview.h
    moofileview/mootreeview.c       moofileview/mootreeview.h
)

# ── mooedit ───────────────────────────────────────────────────────────────────
set(_mooedit_sources
    mooedit/mooedit-enums.h
    mooedit/mooeditor.h         mooedit/mootextview.h
    mooedit/mootextprint.h      mooedit/mooeditview.h
    mooedit/mooeditaction.h     mooedit/mooeditbookmark.h
    mooedit/mooeditconfig.h     mooedit/mooeditdialogs.h
    mooedit/mooeditfiltersettings.h
    mooedit/mooedit.h           mooedit/mooedithistoryitem.h
    mooedit/mooeditprefs.h      mooedit/mooedittypes.h
    mooedit/mooeditwindow.h     mooedit/mooeditfileinfo.h
    mooedit/mooplugin.h         mooedit/mootextbuffer.h
    mooedit/mootextfind.h       mooedit/mootextiter.h
    mooedit/mootextsearch.h     mooedit/mootextstylescheme.h
    mooedit/mooeditview.cpp     mooedit/mooeditview-priv.h
    mooedit/mooeditview-impl.h
    mooedit/mooeditview-script.cpp mooedit/mooeditview-script.h
    mooedit/mooedit-accels.h
    mooedit/mooeditaction.cpp
    mooedit/mooeditaction-factory.cpp mooedit/mooeditaction-factory.h
    mooedit/mooeditbookmark.cpp
    mooedit/mooedit.cpp
    mooedit/mooeditconfig.cpp
    mooedit/mooeditdialogs.cpp
    mooedit/mooedit-enum-types.c mooedit/mooedit-enum-types.h
    mooedit/mooedit-fileops.cpp  mooedit/mooedit-fileops.h
    mooedit/mooeditfiltersettings.cpp
    mooedit/mooedithistoryitem.cpp
    mooedit/mooedit-impl.h
    mooedit/mooedit-script.cpp   mooedit/mooedit-script.h
    mooedit/mooeditprefs.cpp
    mooedit/mooeditprefspage.cpp
    mooedit/mooedit-private.h
    mooedit/mooeditprogress.h    mooedit/mooeditprogress.c
    mooedit/mooedittab.cpp       mooedit/mooedittab.h
    mooedit/mooedittab-impl.h
    mooedit/mooeditwindow.cpp    mooedit/mooeditwindow-impl.h
    mooedit/mooeditfileinfo-impl.h mooedit/mooeditfileinfo.cpp
    mooedit/moofold.cpp          mooedit/moofold.h
    mooedit/mooindenter.cpp      mooedit/mooindenter.h
    mooedit/moospellcheck.cpp    mooedit/moospellcheck.h
    mooedit/moolang.cpp          mooedit/moolang.h
    mooedit/moolangmgr.cpp       mooedit/moolangmgr.h
    mooedit/moolangmgr-private.h mooedit/moolang-private.h
    mooedit/moolinebuffer.c      mooedit/moolinebuffer.h
    mooedit/moolinemark.c        mooedit/moolinemark.h
    mooedit/mooplugin.c
    mooedit/mooplugin-loader.c   mooedit/mooplugin-loader.h
    mooedit/mooplugin-macro.h
    mooedit/mootextbtree.c       mooedit/mootextbtree.h
    mooedit/mootextbuffer.c
    mooedit/mootextfind.c
    mooedit/mootextprint.c       mooedit/mootextprint-private.h
    mooedit/mootext-private.h
    mooedit/mootextsearch.c      mooedit/mootextsearch-private.h
    mooedit/mootextstylescheme.c
    mooedit/mootextview.c        mooedit/mootextview-input.c
    mooedit/mootextview-private.h
    mooedit/mooeditor.cpp        mooedit/mooeditor-impl.h
    mooedit/mooeditor-private.h
    mooedit/mooeditor-tests.cpp  mooedit/mooeditor-tests.h
)

# ── mooapp ────────────────────────────────────────────────────────────────────
set(_mooapp_sources
    mooapp/mooappabout.cpp  mooapp/mooappabout.h
    mooapp/mooapp.cpp       mooapp/mooapp.h
    mooapp/mooapp-accels.h  mooapp/mooapp-info.h
    mooapp/mooapp-private.h
    mooapp/moohtml.h        mooapp/moohtml.cpp
    mooapp/moolinklabel.h   mooapp/moolinklabel.cpp
)

# ── moolua ────────────────────────────────────────────────────────────────────
set(_moolua_sources
    moolua/medit-lua.h      moolua/medit-lua.cpp
    moolua/mooluaplugin.cpp
    moolua/moolua-tests.h   moolua/moolua-tests.cpp
    moolua/moo-lua-api-util.h moolua/moo-lua-api-util.cpp
    moolua/moo-lua-api.cpp  moolua/moo-lua-api.h
    moolua/gtk-lua-api.cpp  moolua/gtk-lua-api.h
    # lua sub-library (compiled in)
    moolua/lua/lfs.h        moolua/lua/lfs.cpp
    moolua/lua/moolua.h     moolua/lua/moolua.cpp
    moolua/lua/luaall.cpp
)

# ── moopython ─────────────────────────────────────────────────────────────────
set(_moopython_base_sources
    moopython/medit-python.h
    moopython/medit-python.c
)
set(_moopython_full_sources
    moopython/pygtk/moo-pygtk.c    moopython/pygtk/moo-pygtk.h
    moopython/moopython-pygtkmod.h
    moopython/moopython-builtin.h   moopython/moopython-builtin.c
    moopython/moopython-api.h
    moopython/moopython-loader.h    moopython/moopython-loader.c
    moopython/moopython-utils.h     moopython/moopython-utils.c
    moopython/moopython-tests.h     moopython/moopython-tests.cpp
)

# ── plugins ───────────────────────────────────────────────────────────────────
set(_plugins_sources
    plugins/moofileselector-prefs.cpp
    plugins/moofileselector.cpp     plugins/moofileselector.h
    plugins/mooplugin-builtin.h     plugins/mooplugin-builtin.cpp
    plugins/moofilelist.cpp
    plugins/moofind.cpp
    # support
    plugins/support/moocmdview.h    plugins/support/moocmdview.cpp
    plugins/support/mooeditwindowoutput.h plugins/support/mooeditwindowoutput.cpp
    plugins/support/moolineview.cpp plugins/support/moolineview.h
    plugins/support/moooutputfilter.cpp   plugins/support/moooutputfilter.h
    # usertools
    plugins/usertools/moousertools.cpp       plugins/usertools/moousertools.h
    plugins/usertools/moousertools-prefs.cpp plugins/usertools/moousertools-prefs.h
    plugins/usertools/moocommand.cpp         plugins/usertools/moocommand.h
    plugins/usertools/moocommand-private.h
    plugins/usertools/moocommanddisplay.cpp  plugins/usertools/moocommanddisplay.h
    plugins/usertools/moooutputfilterregex.cpp plugins/usertools/moooutputfilterregex.h
    plugins/usertools/moousertools-enums.h   plugins/usertools/moousertools-enums.cpp
    plugins/usertools/moocommand-exe.cpp     plugins/usertools/moocommand-exe.h
    plugins/usertools/moocommand-script.cpp  plugins/usertools/moocommand-script.h
)
if(MOO_BUILD_CTAGS)
    list(APPEND _plugins_sources
        plugins/ctags/readtags.c        plugins/ctags/readtags.h
        plugins/ctags/readtags-mangle.h
        plugins/ctags/ctags-plugin.c
        plugins/ctags/ctags-doc.c       plugins/ctags/ctags-doc.h
        plugins/ctags/ctags-view.c      plugins/ctags/ctags-view.h
    )
endif()
if(MOO_BUILD_TERMINAL)
    list(APPEND _plugins_sources plugins/mooterminal.c)
endif()
if(MOO_BUILD_MARKDOWN OR MOO_BUILD_WIKI)
    list(APPEND _plugins_sources plugins/moopagepreview.cpp)
endif()
if(MOO_BUILD_GDB)
    list(APPEND _plugins_sources
        plugins/moogdb/moogdb.cpp
        plugins/moogdb/moogdb-session.cpp
        plugins/moogdb/moogdb-mi-parser.cpp
        plugins/moogdb/moogdb-json.cpp
        plugins/moogdb/moogdb-project.cpp
        plugins/moogdb/moogdb-ui.cpp)
endif()

# ════════════════════════════════════════════════════════════════════════════
# Aggregate all moo_sources (prefix with ${_src}/ for absolute paths)
# ════════════════════════════════════════════════════════════════════════════
set(_all_moo_sources "")
foreach(_rel
    ${_mooglib_sources}
    ${_xdgmime_sources}
    ${_gtksourceview_sources}
    ${_eggsmclient_sources}
    ${_moocpp_sources}
    ${_mooutils_sources}
    ${_moofileview_sources}
    ${_mooedit_sources}
    ${_mooapp_sources}
    ${_moolua_sources}
    ${_moopython_base_sources}
)
    list(APPEND _all_moo_sources "${_src}/${_rel}")
endforeach()

if(MOO_ENABLE_PYTHON)
    foreach(_rel ${_moopython_full_sources})
        list(APPEND _all_moo_sources "${_src}/${_rel}")
    endforeach()
    list(APPEND _all_moo_sources "${_bin}/moopython/pygtk/moo-mod.cpp")
endif()

set(_all_plugins_sources "")
foreach(_rel ${_plugins_sources})
    list(APPEND _all_plugins_sources "${_src}/${_rel}")
endforeach()

# ════════════════════════════════════════════════════════════════════════════
# medit executable   (medit-app/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
if(MOO_BUILD_APP)
    set(_medit_app_sources
        "${_src}/medit-app/main.cpp"
        ${_all_moo_sources}
        ${_all_plugins_sources}
        ${_built_compilable_sources}
        ${_medit_res_src}
    )

    add_executable(medit ${_medit_app_sources})
    add_dependencies(medit moo_codegen)

    # mooutils-misc.cpp and mooapp.cpp use Objective-C++ syntax
    # ([[NSApplication ...]]]) when MOO_USE_QUARTZ is enabled.
    # Tell CMake to compile them as Objective-C++ on macOS Quartz builds.
    if(MOO_USE_QUARTZ)
        set_source_files_properties(
            "${_src}/mooutils/mooutils-misc.cpp"
            "${_src}/mooapp/mooapp.cpp"
            PROPERTIES COMPILE_FLAGS "-x objective-c++"
        )
    endif()

    target_include_directories(medit PRIVATE
        # The generated tree must come first: mooedit/mooeditprefs-general-gxml.h
        # is produced by moo_glade2c into ${_bin}, and a stale copy used to be
        # committed in the source tree.  With ${_src} first, that stale copy won
        # and edits to the .glade file appeared to do nothing.
        "${_bin}"
        "${_src}"
        "${_src}/moofileview"
        "${_src}/moofileview/glade"
        "${_src}/plugins/support"
        "${_top}/doc/built"
        "${CMAKE_BINARY_DIR}"
    )
    if(MOO_ENABLE_PYTHON)
        target_include_directories(medit PRIVATE ${Python3_INCLUDE_DIRS})
    endif()

    # X11 is only needed when GTK actually uses the X11 backend.  On
    # macOS+Quartz (Homebrew default) and Win32, X11::SM / X11::ICE don't
    # exist as imported targets and the link below would error out.
    if(GDK_X11)
        find_package(X11 REQUIRED)
    endif()
    find_package(Threads REQUIRED)
    if(MOO_ENABLE_PYTHON)
        find_package(Python3 COMPONENTS Development REQUIRED)
    endif()

    # Expand library variables explicitly so that INTERFACE compile options
    # from moo_build_settings are NOT passed through to the linker step.
    target_link_libraries(medit PRIVATE
        moo_build_settings          # compile definitions + include dirs only
        PkgConfig::GTK
        PkgConfig::GTHREAD
        PkgConfig::GMODULE
        PkgConfig::XML
        ${MOO_X_LIBS}
        Threads::Threads
        ${MOO_LIBM}
        $<$<BOOL:${MOO_ENABLE_PYTHON}>:PkgConfig::PYGTK>
    )
    if(GDK_X11)
        target_link_libraries(medit PRIVATE X11::X11 X11::SM X11::ICE)
    endif()
    if(MOO_ENABLE_PYTHON)
        target_link_libraries(medit PRIVATE Python3::Python)
    endif()
    if(MOO_BUILD_TERMINAL)
        target_link_libraries(medit PRIVATE PkgConfig::VTE)
    endif()
    if(MOO_BUILD_SPELL)
        target_link_libraries(medit PRIVATE PkgConfig::GSPELL)
    endif()
    if(MOO_BUILD_MARKDOWN)
        target_link_libraries(medit PRIVATE
            PkgConfig::MD4C PkgConfig::MD4C_HTML)
    endif()

    if(MOO_OS_WIN32)
        target_link_options(medit PRIVATE -mwindows)
        # Statically link the MinGW C/C++ runtime + winpthread so the
        # installer doesn't need to ship libgcc_s_seh-1.dll, libstdc++-6.dll,
        # or libwinpthread-1.dll — three DLLs that exist purely to support
        # the compiler-emitted code, not GTK itself.  Saves ~1.7 MB and
        # removes three failure modes for users with mismatched MSYS2
        # runtimes already on PATH.
        target_link_options(medit PRIVATE
            -static-libgcc
            -static-libstdc++
            "LINKER:-Bstatic,--whole-archive,-lwinpthread,--no-whole-archive,-Bdynamic"
        )
        # shlwapi: PathMatchSpecA used by the fnmatch() shim in
        # mooutils/moowin32/mingw/fnmatch.h.
        # ole32:   CoInitializeEx / CoUninitialize used in medit-app/main.cpp.
        # ws2_32:  ntohl/ntohs reached via the netinet/in.h shim that pulls
        #          in <winsock2.h> for xdgmime cache parsing.
        target_link_libraries(medit PRIVATE shlwapi ole32 ws2_32)
    endif()
    if(NOT MOO_OS_WIN32 AND MOO_OS_UNIX)
        target_compile_definitions(medit PRIVATE
            "MOO_UNIT_TEST_DATA_DIR=\"${_top}/moo/medit-app/data\"")
    endif()
    # Enable XSMP for Linux and for macOS+XQuartz (both use the XSMP sources).
    # Do NOT enable for macOS+Quartz (uses eggsmclient-osx.c) or Win32.
    if(NOT MOO_OS_WIN32 AND NOT (MOO_OS_DARWIN AND GDK_QUARTZ))
        target_compile_definitions(medit PRIVATE EGG_SM_CLIENT_BACKEND_XSMP)
    endif()
    if(MOO_USE_QUARTZ)
        target_compile_definitions(medit PRIVATE MOO_USE_QUARTZ)
        target_link_libraries(medit PRIVATE PkgConfig::IGE_MAC)
    endif()

    target_compile_definitions(medit PRIVATE LUA_USE_APICHECK)
    if(MOO_OS_DARWIN)
        target_compile_definitions(medit PRIVATE LUA_USE_MACOSX)
    elseif(MOO_OS_WIN32)
        # no extra lua define
    else()
        target_compile_definitions(medit PRIVATE LUA_USE_POSIX LUA_USE_DLOPEN)
        target_link_libraries(medit PRIVATE dl)
    endif()

    install(TARGETS medit RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

    # gtk-update-icon-cache post-install
    if(MOO_ENABLE_INSTALL_HOOKS)
        find_program(GTK_UPDATE_ICON_CACHE gtk-update-icon-cache)
        if(GTK_UPDATE_ICON_CACHE)
            install(CODE "
                message(STATUS \"Updating GTK icon cache...\")
                execute_process(COMMAND \"${GTK_UPDATE_ICON_CACHE}\" -f -t
                    \"${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor\")
            ")
        endif()
    endif()

    # CTest integration for run-tests-uninstalled.sh.
    # enable_testing()/include(CTest) live at the top level -- called from a
    # subdirectory, `ctest` run from the top of the build tree reported
    # "No tests were found".
    add_test(
        NAME    medit_uninstalled
        COMMAND "${_src}/medit-app/run-tests-uninstalled.sh"
        # run-tests.sh looks for ./medit in the working directory
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:medit>"
    )
    set_tests_properties(medit_uninstalled PROPERTIES
        ENVIRONMENT
            "EXEEXT=;srcdir=${_src};top_srcdir=${_top};bindir=${CMAKE_INSTALL_FULL_BINDIR};PYTHON=${MOO_PYTHON};print_functions=${_src}/medit-app/print-functions.py$<$<NOT:$<BOOL:${MOO_ENABLE_COVERAGE}>>:;IGNORE_COVERAGE=1>"
    )
endif()

# ════════════════════════════════════════════════════════════════════════════
# Python module _moo   (medit-module/Makefile.incl)
# ════════════════════════════════════════════════════════════════════════════
if(MOO_BUILD_MODULE AND MOO_ENABLE_PYTHON)
    set(_module_sources
        "${_src}/medit-module/medit-module.cpp"
        ${_all_moo_sources}
        ${_all_plugins_sources}
        ${_built_compilable_sources}
        ${_moo_module_res_src}
    )

    add_library(_moo MODULE ${_module_sources})
    add_dependencies(_moo moo_codegen)
    set_target_properties(_moo PROPERTIES
        PREFIX ""
        OUTPUT_NAME "_moo"
    )
    if(MOO_OS_WIN32)
        set_target_properties(_moo PROPERTIES SUFFIX ".pyd")
        target_link_options(_moo PRIVATE -no-undefined)
    endif()

    target_include_directories(_moo PRIVATE
        "${_src}" "${_bin}"
        "${_src}/moofileview" "${_src}/moofileview/glade"
        "${_src}/plugins/support"
        "${_top}/doc/built"
        "${CMAKE_BINARY_DIR}"
        ${Python3_INCLUDE_DIRS}
    )
    # Same expansion for the Python module target.
    target_link_libraries(_moo PRIVATE
        moo_build_settings          # compile definitions + include dirs only
        ${GTK_LIBRARIES}
        ${GTHREAD_LIBRARIES}
        ${GMODULE_LIBRARIES}
        ${XML_LIBRARIES}
        ${MOO_X_LIBS}
        ${Python3_LIBRARIES}
        ${MOO_LIBM}
        $<$<BOOL:${MOO_ENABLE_PYTHON}>:${PYGTK_LIBRARIES}>
    )

    install(TARGETS _moo
        LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/python${PYTHON_VERSION}/site-packages"
    )
endif()
