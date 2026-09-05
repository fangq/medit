# cmake/MooVTE.cmake
# Detect libvte-2.91 (the VTE terminal library).
# Sets:
#   MOO_BUILD_TERMINAL  – TRUE if VTE is found and the platform is not Win32
#   HAVE_VTE            – same, spelled the way the autotools build spells it
#   PkgConfig::VTE      – imported target for linking

set(MOO_BUILD_TERMINAL FALSE)
set(HAVE_VTE FALSE)

# VTE is a Unix-only library; skip detection on Windows.
if(MOO_OS_WIN32)
    message(STATUS "VTE terminal: disabled (not available on Win32)")
    return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(VTE IMPORTED_TARGET vte-2.91)

if(VTE_FOUND)
    set(MOO_BUILD_TERMINAL TRUE)
    set(HAVE_VTE TRUE)
    message(STATUS "Found VTE: ${VTE_VERSION} — terminal plugin enabled")
else()
    message(STATUS "VTE (vte-2.91) not found — terminal plugin disabled")
endif()
