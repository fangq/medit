#ifndef __CONFIG_H__
#define __CONFIG_H__

#define MOO_CONFIG_H_INCLUDED 1

/* ── Version ────────────────────────────────────────────────────────────── */
#define MOO_VERSION         "@MOO_VERSION@"
#define MOO_DISPLAY_VERSION "@MOO_DISPLAY_VERSION@"
#define MOO_MAJOR_VERSION    @MOO_MAJOR_VERSION@
#define MOO_MINOR_VERSION    @MOO_MINOR_VERSION@
#define MOO_MICRO_VERSION    @MOO_MICRO_VERSION@
#define MOO_MODULE_MAJOR_VERSION @MOO_MODULE_MAJOR_VERSION@
#define MOO_MODULE_MINOR_VERSION @MOO_MODULE_MINOR_VERSION@

/* ── Contact / copyright ─────────────────────────────────────────────────── */
#define MOO_EMAIL       "@MOO_EMAIL@"
#define MOO_COPYRIGHT   "@MOO_COPYRIGHT@"
#define MOO_WEBSITE     "@MOO_WEBSITE@"
#define MOO_WEB_CONTACT "@MOO_WEB_CONTACT@"
#define PACKAGE_BUGREPORT "@MOO_EMAIL@"

/* ── Package / gettext ───────────────────────────────────────────────────── */
#define MOO_PACKAGE_NAME  "@MOO_PACKAGE_NAME@"
#define GETTEXT_PACKAGE   "@GETTEXT_PACKAGE@"

/* ── File-name constants ─────────────────────────────────────────────────── */
#define MOO_PREFS_XML_FILE_NAME         "@MOO_PREFS_XML_FILE_NAME@"
#define MOO_STATE_XML_FILE_NAME         "@MOO_STATE_XML_FILE_NAME@"
#define MOO_SESSION_XML_FILE_NAME       "@MOO_SESSION_XML_FILE_NAME@"
#define MOO_NAMED_SESSION_XML_FILE_NAME "@MOO_NAMED_SESSION_XML_FILE_NAME@"
#define MEDIT_PORTABLE_MAGIC_FILE_NAME  "@MEDIT_PORTABLE_MAGIC_FILE_NAME@"
#define MEDIT_PORTABLE_DATA_DIR         "@MEDIT_PORTABLE_DATA_DIR@"
#define MEDIT_PORTABLE_CACHE_DIR        "@MEDIT_PORTABLE_CACHE_DIR@"

/* ── Install dirs (Unix only; on Win32 these are runtime-computed) ────────── */
#cmakedefine MOO_DATA_DIR   "@MOO_DATA_DIR@"
#cmakedefine MOO_LIB_DIR    "@MOO_LIB_DIR@"
#cmakedefine MOO_LOCALE_DIR "@MOO_LOCALE_DIR@"
#cmakedefine MOO_HELP_DIR   "@MOO_HELP_DIR@"

/* ── Feature flags ───────────────────────────────────────────────────────── */
/* All sources test these with #ifdef, never #if, so emit "#define X 1" /
   undef instead of "#define X 0".  #ifdef on a defined-but-zero macro is
   true and would activate disabled code paths (e.g. _moo_ctags_plugin_init
   on Windows). */
#cmakedefine ENABLE_NLS              1
#cmakedefine MOO_ENABLE_PYTHON       1
#cmakedefine MOO_BUILD_CTAGS         1
#cmakedefine MOO_BUILD_SPELL         1
#cmakedefine MOO_BUILD_MARKDOWN      1
#cmakedefine MOO_BUILD_WIKI          1
#cmakedefine MOO_BUILD_GDB           1
#cmakedefine MOO_BUILD_TERMINAL      1
#cmakedefine HAVE_VTE                1
#cmakedefine MOO_ENABLE_COVERAGE     1
#cmakedefine MOO_BROKEN_GTK_THEME    1

/* ── OS flags ────────────────────────────────────────────────────────────── */
/* moo-config.h defines MOO_OS_* with value 1, so emit the same form here to
   avoid "redefined" warnings when both headers end up in the same TU. */
#cmakedefine MOO_OS_WIN32   1
#cmakedefine MOO_OS_UNIX    1
#cmakedefine MOO_OS_DARWIN  1
#cmakedefine MOO_OS_FREEBSD 1
#cmakedefine MOO_OS_LINUX   1

/* ── Debug flags ─────────────────────────────────────────────────────────── */
/* moo-config.h requires DEBUG, MOO_DEBUG, and ENABLE_DEBUG to be in sync.   */
/* We define all three together based on the single MOO_ENABLE_DEBUG option.  */
#cmakedefine01 MOO_ENABLE_DEBUG
#if MOO_ENABLE_DEBUG
#  ifndef DEBUG
#    define DEBUG 1
#  endif
#  ifndef MOO_DEBUG
#    define MOO_DEBUG 1
#  endif
#  ifndef ENABLE_DEBUG
#    define ENABLE_DEBUG 1
#  endif
#else
/* Non-debug build: ensure none of the three flags are defined. */
#  undef  DEBUG
#  undef  MOO_DEBUG
#  undef  ENABLE_DEBUG
#endif

/* ── Header availability (replaces autoconf AC_CHECK_HEADERS) ───────────── */
/* Sources test these with #ifdef HAVE_*, so emit "#define X 1" / undef
   instead of "#define X 0".  #ifdef on a defined-but-zero macro evaluates
   true and would pull in headers that aren't actually present (e.g.
   <sys/mman.h> on MinGW where mmap symbols look detectable but the POSIX
   header is missing). */
#cmakedefine HAVE_DLFCN_H        1
#cmakedefine HAVE_INTTYPES_H     1
#cmakedefine HAVE_LOCALE_H       1
#cmakedefine HAVE_SIGNAL_H       1
#cmakedefine HAVE_STDINT_H       1
#cmakedefine HAVE_STDIO_H        1
#cmakedefine HAVE_STDLIB_H       1
#cmakedefine HAVE_STRINGS_H      1
#cmakedefine HAVE_STRING_H       1
#cmakedefine HAVE_SYS_STAT_H     1
#cmakedefine HAVE_SYS_TYPES_H    1
#cmakedefine HAVE_SYS_UTSNAME_H  1
#cmakedefine HAVE_SYS_WAIT_H     1
#cmakedefine HAVE_UNISTD_H       1
#cmakedefine HAVE_WCHAR_H        1
#cmakedefine HAVE_GETC_UNLOCKED  1
#cmakedefine HAVE_MMAP           1

#include <moo-config.h>
#endif /* __CONFIG_H__ */
