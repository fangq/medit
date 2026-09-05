# cmake/MooGtk.cmake
# Equivalent of MOO_PKG_CHECK_GTK_VERSIONS from moo-gtk.m4
# Requires: MooOS.cmake already included

find_package(PkgConfig REQUIRED)

# ── Core GTK / GLib stack ─────────────────────────────────────────────────────
# Minimum versions: GTK 3.22 is the last 3.x feature release and the floor for
# gdk_monitor_*; GLib 2.56 is what Ubuntu 18.04 / RHEL 8 shipped.  These used
# to be unversioned, which meant a build could get surprisingly far on a stack
# far older than anything the code actually supports.
pkg_check_modules(GTK     REQUIRED IMPORTED_TARGET "gtk+-3.0 >= 3.22")
pkg_check_modules(GLIB    REQUIRED IMPORTED_TARGET "glib-2.0 >= 2.56")
pkg_check_modules(GTHREAD REQUIRED IMPORTED_TARGET "gthread-2.0 >= 2.56")
pkg_check_modules(GMODULE REQUIRED IMPORTED_TARGET "gmodule-2.0 >= 2.56")
pkg_check_modules(XML     REQUIRED IMPORTED_TARGET "libxml-2.0 >= 2.9")

# ── GLib minor-version check (mirrors MOO_CHECK_MINOR_VERSION(GLIB,...,[32])) ─
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion glib-2.0
    OUTPUT_VARIABLE _glib_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _m "${_glib_version}")
set(GLIB_MAJOR_VERSION "${CMAKE_MATCH_1}")
set(GLIB_MINOR_VERSION "${CMAKE_MATCH_2}")
set(GLIB_MICRO_VERSION "${CMAKE_MATCH_3}")
message(STATUS "Found GLib: ${_glib_version}")

if(GLIB_MINOR_VERSION GREATER_EQUAL 32)
    set(GLIB_2_32 TRUE)
else()
    set(GLIB_2_32 FALSE)
endif()

# ── GDK backend ──────────────────────────────────────────────────────────────
# Modern GTK3 builds with multiple backends compiled in expose them via
# `--variable=targets` (plural, space-separated, e.g. "x11 broadway wayland").
# Older single-backend builds use `--variable=target`.  Probe both, then check
# each backend independently — GDK_X11/GDK_QUARTZ/GDK_WIN32 are not mutually
# exclusive on multi-backend distros.
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=targets gdk-3.0
    OUTPUT_VARIABLE _gdk_targets
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _gdk_targets)
    execute_process(
        COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=target gdk-3.0
        OUTPUT_VARIABLE _gdk_targets
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()
message(STATUS "GDK targets: ${_gdk_targets}")

set(GDK_X11    FALSE)
set(GDK_WIN32  FALSE)
set(GDK_QUARTZ FALSE)

separate_arguments(_gdk_targets_list NATIVE_COMMAND "${_gdk_targets}")
if("x11"    IN_LIST _gdk_targets_list)
    set(GDK_X11 TRUE)
endif()
if("quartz" IN_LIST _gdk_targets_list)
    set(GDK_QUARTZ TRUE)
endif()
if("win32"  IN_LIST _gdk_targets_list)
    set(GDK_WIN32 TRUE)
endif()

# ── X11 optional packages (only when GDK backend is X11) ─────────────────────
set(MOO_X_CFLAGS "")
set(MOO_X_LIBS   "")

if(GDK_X11)
    foreach(_pkg x11 xext xrender ice sm)
        pkg_check_modules(_XPKG_${_pkg} ${_pkg})
        if(_XPKG_${_pkg}_FOUND)
            list(APPEND MOO_X_CFLAGS ${_XPKG_${_pkg}_CFLAGS})
            list(APPEND MOO_X_LIBS   ${_XPKG_${_pkg}_LIBRARIES})
        endif()
    endforeach()
endif()

# ── Code-generation tools ─────────────────────────────────────────────────────
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=glib_genmarshal glib-2.0
    OUTPUT_VARIABLE GLIB_GENMARSHAL
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=glib_mkenums glib-2.0
    OUTPUT_VARIABLE GLIB_MKENUMS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "glib-genmarshal: ${GLIB_GENMARSHAL}")
message(STATUS "glib-mkenums:    ${GLIB_MKENUMS}")

find_program(GDK_PIXBUF_CSOURCE gdk-pixbuf-csource)
if(NOT GDK_PIXBUF_CSOURCE)
    message(FATAL_ERROR "gdk-pixbuf-csource not found")
endif()
message(STATUS "gdk-pixbuf-csource: ${GDK_PIXBUF_CSOURCE}")

# ── Broken GTK theme workaround ───────────────────────────────────────────────
option(MOO_BROKEN_GTK_THEME "Work around bug in GTK theme (SuSE 9 style)" OFF)
if(MOO_BROKEN_GTK_THEME)
    message(STATUS "Applying broken-gtk-theme workaround")
    # consumers add: target_compile_definitions(... PRIVATE MOO_BROKEN_GTK_THEME=1)
endif()
