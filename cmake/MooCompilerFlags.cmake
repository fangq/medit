# cmake/MooCompilerFlags.cmake
# Equivalent of moo-flags.m4 / MOO_AC_FLAGS / MOO_AC_DEBUG
#
# Provides:
#   moo_apply_compiler_flags(target)   – call after all options are decided

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

# ── Detect compiler identity ─────────────────────────────────────────────────
if(CMAKE_C_COMPILER_ID STREQUAL "Intel")
    set(MOO_ICC TRUE)
    set(MOO_GCC FALSE)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(MOO_ICC FALSE)
    set(MOO_GCC TRUE)
else()
    set(MOO_ICC FALSE)
    set(MOO_GCC FALSE)
endif()

# ── Build-mode options ────────────────────────────────────────────────────────
option(MOO_ENABLE_DEBUG   "Enable debug options"                     OFF)
option(MOO_DEV_MODE       "Enable developer mode (implies debug)"    OFF)
option(MOO_STRICT_MODE    "Enable all warnings + -Werror"            OFF)

# Dev mode inherits debug when not explicitly set
if(MOO_ENABLE_DEBUG AND NOT DEFINED MOO_DEV_MODE_EXPLICIT)
    set(MOO_DEV_MODE ON CACHE BOOL "Enable developer mode" FORCE)
endif()

# ── Helper: test and accumulate a flag ───────────────────────────────────────
function(_moo_check_c_flag flag outvar)
    string(MAKE_C_IDENTIFIER "MOO_C_FLAG${flag}" varname)
    check_c_compiler_flag("${flag}" ${varname})
    if(${varname})
        set(${outvar} "${${outvar}} ${flag}" PARENT_SCOPE)
    endif()
endfunction()

function(_moo_check_cxx_flag flag outvar)
    string(MAKE_C_IDENTIFIER "MOO_CXX_FLAG${flag}" varname)
    check_cxx_compiler_flag("${flag}" ${varname})
    if(${varname})
        set(${outvar} "${${outvar}} ${flag}" PARENT_SCOPE)
    endif()
endfunction()

function(_moo_check_flag flag c_outvar cxx_outvar)
    _moo_check_c_flag("${flag}" ${c_outvar})
    _moo_check_cxx_flag("${flag}" ${cxx_outvar})
    set(${c_outvar}   "${${c_outvar}}"   PARENT_SCOPE)
    set(${cxx_outvar} "${${cxx_outvar}}" PARENT_SCOPE)
endfunction()

# ── Collect flags (called once at configure time) ─────────────────────────────
macro(moo_collect_compiler_flags)
    set(MOO_EXTRA_CFLAGS   "")
    set(MOO_EXTRA_CXXFLAGS "")

    if(MOO_GCC)
        # Deprecation warnings are noisy while GtkAction/GtkUIManager are still
        # in use, so they are silenced by default -- but not in strict mode,
        # which is where the remaining GTK3 deprecations should be visible.
        if(NOT MOO_STRICT_MODE)
            _moo_check_c_flag(-Wno-deprecated-declarations   MOO_EXTRA_CFLAGS)
            _moo_check_cxx_flag(-Wno-deprecated-declarations MOO_EXTRA_CXXFLAGS)
        endif()

        # Common warning/safety flags
        foreach(flag
            -Wall -Wextra -fexceptions -fno-strict-aliasing
            -Wno-missing-field-initializers
            -Wno-format-y2k -Wno-overlength-strings
        )
            _moo_check_c_flag("${flag}"   MOO_EXTRA_CFLAGS)
            _moo_check_cxx_flag("${flag}" MOO_EXTRA_CXXFLAGS)
        endforeach()

        # C++ only flags
        foreach(flag -std=c++11 -fno-rtti)
            _moo_check_cxx_flag("${flag}" MOO_EXTRA_CXXFLAGS)
        endforeach()

        # Debug vs release extras
        if(MOO_ENABLE_DEBUG)
            # (-ftrapv used to be added here; it aborts the process on any
            #  signed overflow in this 2004-era C.  Use -DMOO_SANITIZE=... for
            #  the same diagnosis with a usable report.)
        else()
            _moo_check_cxx_flag(-fno-enforce-eh-specs MOO_EXTRA_CXXFLAGS)
        endif()

        # Strict mode
        if(MOO_STRICT_MODE)
            set(MOO_EXTRA_CFLAGS   "${MOO_EXTRA_CFLAGS} -Werror")
            set(MOO_EXTRA_CXXFLAGS "${MOO_EXTRA_CXXFLAGS} -Werror")

            foreach(flag
                -Wpointer-arith -Wsign-compare -Wreturn-type
                -Wwrite-strings -Wmissing-format-attribute
                -Wdisabled-optimization -Wendif-labels
                -Wvla -Winit-self
            )
                _moo_check_c_flag("${flag}"   MOO_EXTRA_CFLAGS)
                _moo_check_cxx_flag("${flag}" MOO_EXTRA_CXXFLAGS)
            endforeach()

            if(MOO_ENABLE_DEBUG)
                _moo_check_c_flag(-Wlogical-op   MOO_EXTRA_CFLAGS)
                _moo_check_cxx_flag(-Wlogical-op MOO_EXTRA_CXXFLAGS)
            else()
                _moo_check_c_flag(-Wuninitialized   MOO_EXTRA_CFLAGS)
                _moo_check_cxx_flag(-Wuninitialized MOO_EXTRA_CXXFLAGS)
            endif()

            # C-only strict flags
            foreach(flag -Wmissing-prototypes -Wnested-externs -Wno-long-long)
                _moo_check_c_flag("${flag}" MOO_EXTRA_CFLAGS)
            endforeach()

            # C++-only strict flags
            foreach(flag
                -fno-nonansi-builtins -fno-gnu-keywords
                -Wctor-dtor-privacy -Wabi -Wstrict-null-sentinel
                -Woverloaded-virtual -Wsign-promo -Wnon-virtual-dtor
                -Wno-long-long
            )
                _moo_check_cxx_flag("${flag}" MOO_EXTRA_CXXFLAGS)
            endforeach()
        endif()
    endif()

    # ── Preprocessor definitions ──────────────────────────────────────────────
    set(MOO_EXTRA_DEFS "")

    if(MOO_ENABLE_DEBUG)
        list(APPEND MOO_EXTRA_DEFS
            ENABLE_DEBUG ENABLE_PROFILE
            G_ENABLE_DEBUG G_ENABLE_PROFILE
            MOO_DEBUG DEBUG)
    else()
        list(APPEND MOO_EXTRA_DEFS
            NDEBUG=1
            G_DISABLE_CAST_CHECKS
            G_DISABLE_ASSERT)
    endif()

    if(NOT MOO_STRICT_MODE)
        list(APPEND MOO_EXTRA_DEFS GLIB_DISABLE_DEPRECATION_WARNINGS=1)
    endif()
endmacro()

# ── Apply all flags to a target ───────────────────────────────────────────────
function(moo_apply_compiler_flags target)
    separate_arguments(_cflags   UNIX_COMMAND "${MOO_EXTRA_CFLAGS}")
    separate_arguments(_cxxflags UNIX_COMMAND "${MOO_EXTRA_CXXFLAGS}")

    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${_cflags}>
        $<$<COMPILE_LANGUAGE:CXX>:${_cxxflags}>
    )
    target_compile_definitions(${target} PRIVATE ${MOO_EXTRA_DEFS})
endfunction()
