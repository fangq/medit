# cmake/MooPython.cmake
# Equivalent of MOO_AC_PYTHON (moo-pygtk.m4) and MOO_AC_CHECK_PYTHON /
# _MOO_AC_PYTHON_DEVEL (moo-python.m4)
#
# Outputs (all as cache variables / parent-scope vars):
#   MOO_ENABLE_PYTHON    – TRUE/FALSE
#   PYTHON_INCLUDES      – include flags  (e.g. "-I/usr/include/python3.10")
#   PYTHON_LIBS          – linker flags   (e.g. "-L/usr/lib -lpython3.10")
#   PYTHON_VERSION       – version string (e.g. "3.10")
#   PYGTK_INCLUDE_DIRS, PYGTK_LIBRARIES   – from pygobject-3.0

find_package(PkgConfig REQUIRED)

# ── User option ───────────────────────────────────────────────────────────────
set(MOO_WITH_PYTHON "auto" CACHE STRING
    "Build Python support: yes, no, auto, or a specific version like 3.10")
set_property(CACHE MOO_WITH_PYTHON PROPERTY STRINGS auto yes no)

# ── Resolve whether we want Python ───────────────────────────────────────────
set(_moo_want_python "${MOO_WITH_PYTHON}")   # auto | yes | no | version

if(_moo_want_python STREQUAL "no")
    set(MOO_ENABLE_PYTHON FALSE)
    message(STATUS "Python support: disabled by user")
    return()
endif()

# ── Cross-compile / MinGW path ────────────────────────────────────────────────
if(CMAKE_CROSSCOMPILING AND MOO_OS_WIN32)
    # The caller must set PYTHON_INCLUDES, PYTHON_LIBS, PYTHON_VERSION in the
    # toolchain file or on the command line.
    foreach(_var PYTHON_INCLUDES PYTHON_LIBS PYTHON_VERSION)
        if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
            message(FATAL_ERROR
                "Cross-compiling for Win32: please set ${_var} "
                "(e.g. via -D${_var}=... or in your toolchain file)")
        endif()
    endforeach()
    set(MOO_ENABLE_PYTHON TRUE)
    message(STATUS "Python (cross/MinGW): ${PYTHON_VERSION}")
    # Optionally compute pythondir / pyexecdir
    set(PYTHON_PREFIX     "${CMAKE_INSTALL_PREFIX}")
    set(pythondir         "${PYTHON_PREFIX}/lib/python${PYTHON_VERSION}/site-packages")
    set(pyexecdir         "${pythondir}")
    # Still check pygobject below
else()
    # ── Native path: find Python interpreter ─────────────────────────────────
    if(_moo_want_python MATCHES "^[0-9]")
        # specific version requested
        find_package(Python3 "${_moo_want_python}" EXACT
            COMPONENTS Interpreter Development)
    else()
        # 3.6 went EOL in 2021; 3.8 is the oldest still seen in the wild.
        find_package(Python3 3.8 COMPONENTS Interpreter Development)
    endif()

    if(NOT Python3_FOUND)
        if(_moo_want_python STREQUAL "yes")
            message(FATAL_ERROR "Python support requested but Python >= 3.6 not found")
        else()
            set(MOO_ENABLE_PYTHON FALSE)
            message(WARNING "Python not found – disabling Python support")
            return()
        endif()
    endif()

    set(PYTHON_VERSION   "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
    # Build PYTHON_INCLUDES / PYTHON_LIBS in the same form the old m4 did
    set(PYTHON_INCLUDES  "-I${Python3_INCLUDE_DIRS}")
    set(PYTHON_LIBS      "-L${Python3_LIBRARY_DIRS} -lpython${PYTHON_VERSION}")
    message(STATUS "Found Python: ${Python3_EXECUTABLE} (${PYTHON_VERSION})")
    message(STATUS "  includes: ${PYTHON_INCLUDES}")
    message(STATUS "  libs:     ${PYTHON_LIBS}")
endif()

# ── Check for pygobject-3.0 ≥ 3.30 (replaces PKG_CHECK_MODULES PYGTK) ───────
# Only mark the dependency REQUIRED when the user explicitly asked for Python
# (MOO_WITH_PYTHON=yes).  In "auto" mode (the default) and on Win32 cross-
# compile, treat pygobject as optional so the build degrades gracefully to
# no-Python instead of failing the whole configure step.
if(_moo_want_python STREQUAL "yes" AND
   NOT (CMAKE_CROSSCOMPILING AND MOO_OS_WIN32))
    pkg_check_modules(PYGTK REQUIRED IMPORTED_TARGET pygobject-3.0>=3.30)
else()
    pkg_check_modules(PYGTK IMPORTED_TARGET pygobject-3.0>=3.30)
endif()

if(NOT PYGTK_FOUND)
    if(_moo_want_python STREQUAL "yes" AND NOT (CMAKE_CROSSCOMPILING AND MOO_OS_WIN32))
        message(FATAL_ERROR
            "Python support requested but pygobject-3.0 >= 3.30 not found")
    else()
        set(MOO_ENABLE_PYTHON FALSE)
        message(WARNING "pygobject-3.0 not found – disabling Python support")
        return()
    endif()
endif()

set(MOO_ENABLE_PYTHON TRUE)
message(STATUS "Python support: enabled (pygobject ${PYGTK_VERSION})")
message(STATUS "  pygobject include dirs: ${PYGTK_INCLUDE_DIRS}")
message(STATUS "  pygobject libraries:    ${PYGTK_LIBRARIES}")

# Alias PYGTK_* -> PYGOBJECT_* so both names work in the rest of the build.
# pkg_check_modules(PYGTK ...) populates PYGTK_INCLUDE_DIRS / PYGTK_LIBRARIES;
# other parts of the CMake files use PYGOBJECT_INCLUDE_DIRS / PYGOBJECT_LIBRARIES.
set(PYGOBJECT_INCLUDE_DIRS ${PYGTK_INCLUDE_DIRS})
set(PYGOBJECT_LIBRARIES    ${PYGTK_LIBRARIES})

# PYGTK_DEFS_DIR / PYGOBJECT_DEFS_DIR – kept empty as in original
set(PYGTK_DEFS_DIR     "")
set(PYGOBJECT_DEFS_DIR "")
