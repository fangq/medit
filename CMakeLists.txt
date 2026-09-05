cmake_minimum_required(VERSION 3.16)

# ── Version metadata ──────────────────────────────────────────────────────────
set(MOO_MAJOR_VERSION  1)
set(MOO_MINOR_VERSION  8)
set(MOO_MICRO_VERSION  0)
set(MOO_VERSION_SUFFIX "devel")          # set to "" for a release build
set(MOO_MODULE_MAJOR_VERSION 2)
set(MOO_MODULE_MINOR_VERSION 0)

set(MOO_VERSION "${MOO_MAJOR_VERSION}.${MOO_MINOR_VERSION}.${MOO_MICRO_VERSION}")
if(MOO_VERSION_SUFFIX)
    set(MOO_DISPLAY_VERSION "${MOO_VERSION}-${MOO_VERSION_SUFFIX}")
else()
    set(MOO_DISPLAY_VERSION "${MOO_VERSION}")
endif()

# Project contact / copyright.
#
# medit is a fork: Yevgen Muntyan wrote the original and keeps the authorship
# credit, but he does not maintain this tree, so the contact points and the
# package maintainer must not name him.  These feed PACKAGE_BUGREPORT, the
# About dialog, the Windows installer and the CPack .deb/.dmg metadata.
set(MOO_WEBSITE     "https://github.com/fangq/medit")
set(MOO_WEB_CONTACT "https://github.com/fangq/medit/issues")
set(MOO_EMAIL       "q.fang@neu.edu")
set(MOO_MAINTAINER  "Qianqian Fang <${MOO_EMAIL}>")
set(MOO_COPYRIGHT   "2004-2010 Yevgen Muntyan, 2025-2026 Qianqian Fang <${MOO_EMAIL}>")

project(medit
    VERSION  ${MOO_MAJOR_VERSION}.${MOO_MINOR_VERSION}.${MOO_MICRO_VERSION}
    LANGUAGES C CXX
)

# ── Build type ────────────────────────────────────────────────────────────────
# Without a default, "cmake .. && make" (what the README tells people to run)
# built with neither -O nor -g, while still defining NDEBUG/G_DISABLE_ASSERT.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to RelWithDebInfo")
endif()

# ── Language standards ────────────────────────────────────────────────────────
# C was previously left at whatever the compiler defaults to -- gcc 14 defaults
# to gnu23, which this 2004-era C is not written for.  C++ was pinned only by a
# raw -std=c++11 flag in MooCompilerFlags.cmake.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# C++14, not 17: the vendored Lua (moo/moolua/lua) still uses the `register`
# storage class, which C++17 removed.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Module search path ────────────────────────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Required build tools ──────────────────────────────────────────────────────
# A hardcoded list of point releases rots; python3 is enough on any platform
# that has one, and "python" covers the rest.
find_program(MOO_PYTHON NAMES python3 python)
if(NOT MOO_PYTHON)
    message(FATAL_ERROR "Python interpreter not found; it is required for the build")
endif()
message(STATUS "Build Python: ${MOO_PYTHON}")

find_program(WINDRES windres)
find_program(TXT2TAGS txt2tags)
find_program(XSLTPROC  xsltproc)

# ── Package / install-dir constants ──────────────────────────────────────────
set(MOO_PACKAGE_NAME  "medit-1")
set(GETTEXT_PACKAGE       "${MOO_PACKAGE_NAME}")
set(GETTEXT_PACKAGE_GSV   "${MOO_PACKAGE_NAME}-gsv")

include(GNUInstallDirs)   # gives CMAKE_INSTALL_DATADIR, LIBDIR, etc.

set(MOO_DATA_DIR        "${CMAKE_INSTALL_FULL_DATADIR}/${MOO_PACKAGE_NAME}")
set(MOO_LIB_DIR         "${CMAKE_INSTALL_FULL_LIBDIR}/${MOO_PACKAGE_NAME}")
set(MOO_DOC_DIR         "${CMAKE_INSTALL_FULL_DATADIR}/doc/${MOO_PACKAGE_NAME}")
set(MOO_HELP_DIR        "${MOO_DOC_DIR}/help")
set(MOO_TEXT_LANG_FILES_DIR "${MOO_DATA_DIR}/language-specs")
set(MOO_PYTHON_PLUGIN_DIR   "${MOO_DATA_DIR}/plugins")
set(MOO_PYTHON_LIB_DIR      "${MOO_DATA_DIR}/python")
set(MOO_LOCALE_DIR      "${CMAKE_INSTALL_FULL_LOCALEDIR}")

# File-name constants (go into config.h)
set(MOO_PREFS_XML_FILE_NAME          "prefs.xml")
set(MOO_STATE_XML_FILE_NAME          "state.xml")
set(MOO_NAMED_SESSION_XML_FILE_NAME  "session-%s.xml")
set(MOO_SESSION_XML_FILE_NAME        "session.xml")
set(MEDIT_PORTABLE_MAGIC_FILE_NAME   "medit-portable")
set(MEDIT_PORTABLE_DATA_DIR          "medit-portable-data")
set(MEDIT_PORTABLE_CACHE_DIR         "medit-portable-cache")

# ── OS detection ──────────────────────────────────────────────────────────────
include(MooOS)

# ── Compiler flags / debug / strict options ───────────────────────────────────
include(MooCompilerFlags)
moo_collect_compiler_flags()   # sets MOO_EXTRA_CFLAGS, MOO_EXTRA_CXXFLAGS, MOO_EXTRA_DEFS

# System-extensions equivalent of AC_USE_SYSTEM_EXTENSIONS
if(UNIX)
    add_compile_definitions(_GNU_SOURCE)
endif()

# ── macOS Homebrew prefix + optional ige-mac-integration ─────────────────────
if(MOO_OS_DARWIN)
    include(MooMacOS)   # sets MOO_HOMEBREW_PREFIX, MOO_USE_QUARTZ
endif()

# ── GTK / GLib / libxml2 ─────────────────────────────────────────────────────
include(MooGtk)

# ── NLS / intltool / gettext ─────────────────────────────────────────────────
include(MooNLS)

# ── VTE terminal library ──────────────────────────────────────────────────────
include(MooVTE)   # sets MOO_BUILD_TERMINAL and PkgConfig::VTE

# ── Header / function checks (replaces autoconf AC_CHECK_HEADERS) ─────────────
include(CheckIncludeFile)
include(CheckFunctionExists)

check_include_file("dlfcn.h" HAVE_DLFCN_H)
check_include_file("inttypes.h" HAVE_INTTYPES_H)
check_include_file("locale.h" HAVE_LOCALE_H)
check_include_file("signal.h" HAVE_SIGNAL_H)
check_include_file("stdint.h" HAVE_STDINT_H)
check_include_file("stdio.h" HAVE_STDIO_H)
check_include_file("stdlib.h" HAVE_STDLIB_H)
check_include_file("strings.h" HAVE_STRINGS_H)
check_include_file("string.h" HAVE_STRING_H)
check_include_file("sys/stat.h" HAVE_SYS_STAT_H)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
check_include_file("sys/utsname.h" HAVE_SYS_UTSNAME_H)
check_include_file("sys/wait.h" HAVE_SYS_WAIT_H)
check_include_file("unistd.h" HAVE_UNISTD_H)
check_include_file("wchar.h" HAVE_WCHAR_H)

check_function_exists(getc_unlocked HAVE_GETC_UNLOCKED)

# HAVE_MMAP must require BOTH the function and <sys/mman.h>: MinGW exposes
# an mmap symbol via the runtime but doesn't ship the POSIX header that the
# xdgmime cache code unconditionally #include's behind #ifdef HAVE_MMAP.
check_function_exists(mmap _have_mmap_func)
check_include_file("sys/mman.h" _have_sys_mman_h)
if(_have_mmap_func AND _have_sys_mman_h)
    set(HAVE_MMAP 1)
endif()

# ── Python bindings ───────────────────────────────────────────────────────────
include(MooPython)  # sets MOO_ENABLE_PYTHON, PYTHON_INCLUDES, PYTHON_LIBS, PYGTK_*

# ── Spell-check (gspell + hunspell) ───────────────────────────────────────────
set(MOO_WITH_SPELL "auto" CACHE STRING
    "Build with gspell-based spell-check: yes, no, or auto")
set_property(CACHE MOO_WITH_SPELL PROPERTY STRINGS auto yes no)
include(MooSpell)   # sets MOO_BUILD_SPELL, PkgConfig::GSPELL

# ── Markdown preview (md4c + md4c-html) ──────────────────────────────────────
set(MOO_WITH_MARKDOWN "auto" CACHE STRING
    "Build with md4c-based Markdown preview pane: yes, no, or auto")
set_property(CACHE MOO_WITH_MARKDOWN PROPERTY STRINGS auto yes no)
include(MooMarkdown)   # sets MOO_BUILD_MARKDOWN, PkgConfig::MD4C{,_HTML}

# ── Wiki preview (in-tree converter, no external deps) ───────────────────────
set(MOO_WITH_WIKI "auto" CACHE STRING
    "Build with Habitat/UseMod-style wiki preview pane: yes, no, or auto")
set_property(CACHE MOO_WITH_WIKI PROPERTY STRINGS auto yes no)
include(MooWiki)   # sets MOO_BUILD_WIKI (always TRUE unless explicitly disabled)

# ── GDB-based C/C++ debugger (drives `gdb --interpreter=mi3` at runtime) ─────
set(MOO_WITH_GDB "auto" CACHE STRING
    "Build with the GDB-driven debugger plugin: yes, no, or auto")
set_property(CACHE MOO_WITH_GDB PROPERTY STRINGS auto yes no)
include(MooGdb)   # sets MOO_BUILD_GDB (TRUE unless explicitly disabled)

# ── Build-target options ──────────────────────────────────────────────────────
option(MOO_BUILD_MODULE "Build standalone Python module instead of medit app" OFF)

if(MOO_BUILD_MODULE)
    if(NOT MOO_ENABLE_PYTHON)
        message(FATAL_ERROR "Python bindings are not enabled; can't build Python module")
    endif()
    if(NOT BUILD_SHARED_LIBS)
        message(FATAL_ERROR "Python module requires -DBUILD_SHARED_LIBS=ON")
    endif()
    set(MOO_BUILD_APP OFF)
else()
    set(MOO_BUILD_APP ON)
endif()

# ctags plugin (Unix-only, mirrors AM_CONDITIONAL(MOO_BUILD_CTAGS, $MOO_OS_UNIX))
set(MOO_BUILD_CTAGS ${MOO_OS_UNIX})

# ── Coverage ──────────────────────────────────────────────────────────────────
option(MOO_ENABLE_COVERAGE "Enable code-coverage instrumentation" OFF)
if(MOO_ENABLE_COVERAGE)
    add_compile_options(--coverage)
    add_link_options(--coverage)
endif()

# ── Testing ───────────────────────────────────────────────────────────────────
# Must be at the top level: enable_testing() called from moo/CMakeLists.txt only
# registered tests for that subdirectory, so `ctest` run from the build root
# found none.
include(CTest)
enable_testing()

# ── Sanitizers ────────────────────────────────────────────────────────────────
# e.g. cmake .. -DCMAKE_BUILD_TYPE=Debug -DMOO_SANITIZE=address,undefined
set(MOO_SANITIZE "" CACHE STRING
    "Comma-separated -fsanitize= list (e.g. address,undefined); empty to disable")
if(MOO_SANITIZE)
    message(STATUS "Sanitizers: ${MOO_SANITIZE}")
    add_compile_options(-fsanitize=${MOO_SANITIZE}
                        -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${MOO_SANITIZE})
endif()

# ── Install hooks (gtk-update-icon-cache) ─────────────────────────────────────
if(MOO_BUILD_APP)
    option(MOO_ENABLE_INSTALL_HOOKS
        "Run gtk-update-icon-cache during install" ON)
else()
    option(MOO_ENABLE_INSTALL_HOOKS
        "Run gtk-update-icon-cache during install" OFF)
endif()

# ── Windows / Inno Setup ─────────────────────────────────────────────────────
if(MOO_OS_WIN32)
    set(MEDIT_WIN32_APP_UID "7F9F899F-EE8A-47F0-9981-8C525AF78E4D")

    if(CMAKE_HOST_WIN32)
        # Native Windows build (MSYS2 / MinGW with cmake.exe).  Look for
        # ISCC.exe in the standard Inno Setup install locations.
        # Note: $ENV{ProgramFiles(x86)} can't be expanded by CMake (the
        # parentheses break variable-name parsing), so list the well-known
        # absolute paths directly instead.
        find_program(MEDIT_ISCC
            NAMES ISCC.exe iscc
            PATHS "C:/Program Files (x86)/Inno Setup 6"
                  "C:/Program Files/Inno Setup 6"
                  "C:/Program Files (x86)/Inno Setup 5"
                  "C:/Program Files/Inno Setup 5"
        )
        # Don't wrap the path in literal " — CMake passes a single-string
        # variable as one argv element to add_custom_target(COMMAND ...) and
        # rejects values containing literal quote characters.
        if(MEDIT_ISCC)
            set(MEDIT_INNO_COMPILER "${MEDIT_ISCC}")
        else()
            set(MEDIT_INNO_COMPILER "ISCC.exe")
        endif()
        set(MEDIT_INNO_TOP_SRCDIR   "${CMAKE_SOURCE_DIR}")
        set(MEDIT_INNO_TOP_BUILDDIR "${CMAKE_BINARY_DIR}")
        set(MEDIT_INNO_INSTDIR      "${CMAKE_INSTALL_PREFIX}")
    elseif(CYGWIN)
        set(MEDIT_INNO_COMPILER
            "\"/cygdrive/c/Program Files (x86)/Inno Setup 5/ISCC.exe\"")
        set(MEDIT_INNO_TOP_SRCDIR   "$(shell cygpath -m ${CMAKE_SOURCE_DIR})")
        set(MEDIT_INNO_TOP_BUILDDIR "$(shell cygpath -m ${CMAKE_BINARY_DIR})")
        set(MEDIT_INNO_INSTDIR      "$(shell cygpath -m ${CMAKE_INSTALL_PREFIX})")
    elseif(EXISTS "$ENV{HOME}/.wine/drive_c/windows/syswow64")
        set(MEDIT_INNO_COMPILER
            "wine \"c:\\\\program files (x86)\\\\inno setup 5\\\\ISCC.exe\"")
        set(MEDIT_INNO_TOP_SRCDIR   "Z:${CMAKE_SOURCE_DIR}")
        set(MEDIT_INNO_TOP_BUILDDIR "Z:${CMAKE_BINARY_DIR}")
        set(MEDIT_INNO_INSTDIR      "Z:${CMAKE_INSTALL_PREFIX}")
    else()
        set(MEDIT_INNO_COMPILER
            "wine \"c:\\\\program files\\\\inno setup 5\\\\ISCC.exe\"")
        set(MEDIT_INNO_TOP_SRCDIR   "Z:${CMAKE_SOURCE_DIR}")
        set(MEDIT_INNO_TOP_BUILDDIR "Z:${CMAKE_BINARY_DIR}")
        set(MEDIT_INNO_INSTDIR      "Z:${CMAKE_INSTALL_PREFIX}")
    endif()
    message(STATUS "Inno compiler:           ${MEDIT_INNO_COMPILER}")
    message(STATUS "MEDIT_INNO_TOP_SRCDIR:   ${MEDIT_INNO_TOP_SRCDIR}")
    message(STATUS "MEDIT_INNO_TOP_BUILDDIR: ${MEDIT_INNO_TOP_BUILDDIR}")
    message(STATUS "MEDIT_INNO_INSTDIR:      ${MEDIT_INNO_INSTDIR}")

    if(MOO_ENABLE_DEBUG)
        set(MOO_WIN32_CONFIG "debug")
    else()
        set(MOO_WIN32_CONFIG "release")
    endif()
    message(STATUS "Win32 build configuration: ${MOO_WIN32_CONFIG}")

    if(MOO_WIN32_CONFIG STREQUAL "release")
        set(MEDIT_INNO_INSTALLER_SUFFIX "")
    else()
        set(MEDIT_INNO_INSTALLER_SUFFIX "-${MOO_WIN32_CONFIG}")
    endif()
    set(MEDIT_SETUP_NAME
        "medit-${MOO_DISPLAY_VERSION}${MEDIT_INNO_INSTALLER_SUFFIX}")
    message(STATUS "Win32 setup name: ${MEDIT_SETUP_NAME}")
endif()

# ── Global INTERFACE target ───────────────────────────────────────────────────
# Replaces MOO_CFLAGS / MOO_CPPFLAGS / MOO_LIBS AC_SUBST variables.
# Subdirectories link against this target instead of repeating flags.
add_library(moo_build_settings INTERFACE)

# Note: PkgConfig::GTK/GLIB/etc. imported targets propagate their own
# include directories, so we don't need to list *_INCLUDE_DIRS separately.
target_include_directories(moo_build_settings INTERFACE
    "${CMAKE_BINARY_DIR}"                   # generated config.h
    "${CMAKE_SOURCE_DIR}/moo"
    ${MOO_X_CFLAGS}
    $<$<BOOL:${MOO_ENABLE_PYTHON}>:${PYGTK_INCLUDE_DIRS}>
    $<$<BOOL:${MOO_ENABLE_PYTHON}>:${Python3_INCLUDE_DIRS}>
)

target_compile_definitions(moo_build_settings INTERFACE
    HAVE_CONFIG_H=1
    MOO_CONFIG_H_INCLUDED=1
    GETTEXT_PACKAGE="${GETTEXT_PACKAGE}"
    XDG_PREFIX=_moo_edit_xdg
    "G_LOG_DOMAIN=\"Moo\""
    ${MOO_EXTRA_DEFS}
    $<$<BOOL:${MOO_ENABLE_PYTHON}>:MOO_ENABLE_PYTHON=1>
    $<$<BOOL:${MOO_BUILD_CTAGS}>:MOO_BUILD_CTAGS=1>
    $<$<BOOL:${MOO_BUILD_SPELL}>:MOO_BUILD_SPELL=1>
    $<$<BOOL:${MOO_BUILD_MARKDOWN}>:MOO_BUILD_MARKDOWN=1>
    $<$<BOOL:${MOO_BUILD_WIKI}>:MOO_BUILD_WIKI=1>
    $<$<BOOL:${MOO_BUILD_GDB}>:MOO_BUILD_GDB=1>
    $<$<BOOL:${MOO_BUILD_TERMINAL}>:MOO_BUILD_TERMINAL=1>
    $<$<BOOL:${MOO_ENABLE_COVERAGE}>:MOO_ENABLE_COVERAGE=1>
    $<$<BOOL:${MOO_BROKEN_GTK_THEME}>:MOO_BROKEN_GTK_THEME=1>
)

if(MOO_OS_WIN32)
    target_compile_definitions(moo_build_settings INTERFACE
        UNICODE _UNICODE STRICT WIN32_LEAN_AND_MEAN
        U__STRICT_ANSI__
    )
    if(MOO_OS_MINGW)
        target_include_directories(moo_build_settings INTERFACE
            "${CMAKE_SOURCE_DIR}/moo/mooutils/moowin32/mingw")
    endif()
endif()

if(MOO_OS_UNIX)
    target_compile_definitions(moo_build_settings INTERFACE
        "MOO_DATA_DIR=\"${MOO_DATA_DIR}\""
        "MOO_LIB_DIR=\"${MOO_LIB_DIR}\""
        "MOO_LOCALE_DIR=\"${MOO_LOCALE_DIR}\""
        "MOO_HELP_DIR=\"${MOO_HELP_DIR}\""
    )
endif()

if(NOT MOO_STRICT_MODE)
    target_compile_definitions(moo_build_settings INTERFACE
        GLIB_DISABLE_DEPRECATION_WARNINGS=1)
endif()

# Convert space-separated flag strings from MooCompilerFlags into proper
# CMake lists (semicolon-separated) so they can be passed individually.
# This avoids SHELL: which can leak flags into the linker command line.
separate_arguments(_moo_c_flags_list   UNIX_COMMAND "${MOO_EXTRA_CFLAGS}")
separate_arguments(_moo_cxx_flags_list UNIX_COMMAND "${MOO_EXTRA_CXXFLAGS}")
target_compile_options(moo_build_settings INTERFACE
    $<$<COMPILE_LANGUAGE:C>:${_moo_c_flags_list}>
    $<$<COMPILE_LANGUAGE:CXX>:${_moo_cxx_flags_list}>
)

target_link_libraries(moo_build_settings INTERFACE
    ${GTK_LIBRARIES}
    ${GTHREAD_LIBRARIES}
    ${GMODULE_LIBRARIES}
    ${XML_LIBRARIES}
    ${MOO_X_LIBS}
    ${MOO_LIBM}
    $<$<BOOL:${MOO_ENABLE_PYTHON}>:${PYGTK_LIBRARIES}>
)

# pkg_check_modules' *_LIBRARIES are bare names (gtk-3, glib-2.0, ...).
# Without the matching -L, macOS Homebrew (/opt/homebrew/lib) and other
# non-default prefixes fail with "library 'gtk-3' not found".  PkgConfig::*
# imported targets carry the -L too, but they may be evaluated after these
# bare names in the link line, so add the dirs explicitly here.
target_link_directories(moo_build_settings INTERFACE
    ${GTK_LIBRARY_DIRS}
    ${GTHREAD_LIBRARY_DIRS}
    ${GMODULE_LIBRARY_DIRS}
    ${XML_LIBRARY_DIRS}
)

# ── Generate config.h ─────────────────────────────────────────────────────────
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/config.h"
)

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(api)
add_subdirectory(doc)
add_subdirectory(moo)
add_subdirectory(po)
add_subdirectory(po-gsv)

# ── Windows installer / portable targets (from root-Makefile.am) ──────────────
if(MOO_OS_WIN32)
    # Generate installer.iss from template
    configure_file(
        "${CMAKE_SOURCE_DIR}/plat/win32/installer.iss.in"
        "${CMAKE_BINARY_DIR}/plat/win32/installer.iss"
        @ONLY
    )

    add_custom_target(installer
        COMMAND ${CMAKE_COMMAND} --build . --target install
        COMMAND ${MEDIT_INNO_COMPILER} "${CMAKE_BINARY_DIR}/plat/win32/installer.iss"
        COMMENT "Building Win32 installer"
    )

    add_custom_target(portable
        COMMAND ${CMAKE_COMMAND} --build . --target install
        COMMAND ${CMAKE_COMMAND} -E remove_directory medit-win-dist-tmp
        COMMAND ${CMAKE_COMMAND} -E make_directory   medit-win-dist-tmp
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_INSTALL_PREFIX}"
                "medit-win-dist-tmp/medit-${MOO_DISPLAY_VERSION}"
        COMMAND bash -c
            "cd medit-win-dist-tmp && zip -r9 medit-${MOO_DISPLAY_VERSION}.zip medit-${MOO_DISPLAY_VERSION}"
        COMMAND bash -c
            "echo 'This file enables portable mode for medit' > medit-win-dist-tmp/medit-${MOO_DISPLAY_VERSION}/bin/${MEDIT_PORTABLE_MAGIC_FILE_NAME}"
        COMMAND bash -c
            "cd medit-win-dist-tmp && zip -r9 medit-portable-${MOO_DISPLAY_VERSION}.zip medit-${MOO_DISPLAY_VERSION}"
        # cmake -E rename needs a full destination filename, not a directory
        # — passing "." here used to silently fall through under autotools'
        # `mv X .` semantics but fails outright with the CMake driver.
        COMMAND ${CMAKE_COMMAND} -E rename
                "medit-win-dist-tmp/medit-portable-${MOO_DISPLAY_VERSION}.zip"
                "medit-portable-${MOO_DISPLAY_VERSION}.zip"
        COMMAND ${CMAKE_COMMAND} -E rename
                "medit-win-dist-tmp/medit-${MOO_DISPLAY_VERSION}.zip"
                "medit-${MOO_DISPLAY_VERSION}.zip"
        COMMAND ${CMAKE_COMMAND} -E remove_directory medit-win-dist-tmp
        COMMENT "Building Win32 portable zip"
    )

    add_custom_target(bdist DEPENDS installer portable)
endif()

# ── Convenience test targets (from root-Makefile.am) ─────────────────────────
add_custom_target(fullcheck
    COMMAND ${CMAKE_COMMAND} --build . --target all
    COMMAND ${CMAKE_CTEST_COMMAND}
    COMMENT "Full check: build + test"
)

# ── CPack: source/binary packaging (used by GitHub Actions for .deb/.tar.gz) ─
set(CPACK_PACKAGE_NAME              "medit")
set(CPACK_PACKAGE_VERSION           "${MOO_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "GTK3 text editor with syntax highlighting and embedded terminal")
set(CPACK_PACKAGE_VENDOR            "medit project")
set(CPACK_PACKAGE_CONTACT           "${MOO_EMAIL}")
set(CPACK_PACKAGE_HOMEPAGE_URL      "${MOO_WEBSITE}")
set(CPACK_RESOURCE_FILE_LICENSE     "${CMAKE_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "medit")

if(MOO_OS_LINUX)
    set(CPACK_GENERATOR                 "DEB;TGZ")
    # Debian requires "Name <email>", not a bare address.
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${MOO_MAINTAINER}")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "editors")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
    # Auto-detect shared-library deps (libgtk-3, libvte, libxml2, ...).
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)
    set(CPACK_DEBIAN_FILE_NAME          DEB-DEFAULT)
    # Recommend the en_US hunspell dictionary so spell-check works
    # out of the box.  Recommends rather than Depends so users on
    # other locales aren't forced to pull in English.
    if(MOO_BUILD_SPELL)
        set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "hunspell-en-us")
    endif()
    # The project's install() rules use CMAKE_INSTALL_FULL_DATADIR (absolute)
    # for some destinations, so CPack must use DESTDIR to redirect them into
    # the staging tree.  Without this it tries to write to /usr/... directly.
    set(CPACK_SET_DESTDIR               ON)
elseif(MOO_OS_DARWIN)
    set(CPACK_GENERATOR                 "TGZ")
endif()

include(CPack)
