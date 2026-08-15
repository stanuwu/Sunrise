# CMake wrapper for Sunrise
#
# This file delegates to `zig build` to produce the Windows DLL and import
# library. It exists so that CMake-based projects can consume Sunrise without
# interacting with the Zig build system directly. However, downstream users do
# still require `zig` on the PATH.
#
# Building within the Sunrise repo
# --------------------------------
#
#   cmake -B build
#   cmake --build build
#   cmake --install build --prefix /usr/local
#
# Pass extra flags to the Zig build with SUNRISE_ZIG_BUILD_FLAGS:
#
#   cmake -B build -DSUNRISE_ZIG_BUILD_FLAGS="-Dxwin-dir=/path/to/xwin"
#
# Integrating into a downstream CMake project
# --------------------------------------------
#
# FetchContent (no manual install step):
#
#   include(FetchContent)
#   FetchContent_Declare(sunrise
#       GIT_REPOSITORY https://github.com/stanuwu/Sunrise.git
#       GIT_TAG main
#   )
#   FetchContent_MakeAvailable(sunrise)
#
#   target_link_libraries(myapp PRIVATE sunrise)

cmake_minimum_required(VERSION 3.19)
project(Sunrise VERSION 0.2.1 LANGUAGES C)

# --- Options ----------------------------------------------------------------

set(SUNRISE_ZIG_BUILD_FLAGS "" CACHE STRING "Additional flags to pass to zig build")

# Map CMake build types to Zig optimization levels.
set(_SUNRISE_ZIG_OPT_FLAG "")
if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
    if(_bt STREQUAL "RELEASE" OR _bt STREQUAL "MINSIZEREL" OR _bt STREQUAL "RELWITHDEBINFO")
        set(_SUNRISE_ZIG_OPT_FLAG "-Doptimize=ReleaseFast")
    endif()
    unset(_bt)
endif()

if(_SUNRISE_ZIG_OPT_FLAG)
    list(APPEND SUNRISE_ZIG_BUILD_FLAGS "${_SUNRISE_ZIG_OPT_FLAG}")
endif()

# --- Find Zig ----------------------------------------------------------------

find_program(ZIG_EXECUTABLE zig REQUIRED)
message(STATUS "Found zig: ${ZIG_EXECUTABLE}")

# --- Build via zig build -----------------------------------------------------

# The Zig build installs into zig-out/ relative to the source tree.
set(ZIG_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zig-out")
set(SUNRISE_SHARED_LIBRARY "${ZIG_OUT_DIR}/bin/steam_api64.dll")
set(SUNRISE_IMPORT_LIBRARY "${ZIG_OUT_DIR}/lib/steam_api64.lib")

add_custom_command(
    OUTPUT "${SUNRISE_SHARED_LIBRARY}" "${SUNRISE_IMPORT_LIBRARY}"
    COMMAND "${ZIG_EXECUTABLE}" build ${SUNRISE_ZIG_BUILD_FLAGS}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building Sunrise via zig build..."
    USES_TERMINAL
)

add_custom_target(zig_build_sunrise ALL
    DEPENDS "${SUNRISE_SHARED_LIBRARY}" "${SUNRISE_IMPORT_LIBRARY}"
)

# Tell CMake's clean target to also remove Zig's output directory.
set_property(DIRECTORY APPEND PROPERTY
    ADDITIONAL_CLEAN_FILES "${ZIG_OUT_DIR}"
)

# --- IMPORTED library target -------------------------------------------------

add_library(sunrise SHARED IMPORTED GLOBAL)
set_target_properties(sunrise PROPERTIES
    IMPORTED_LOCATION "${SUNRISE_SHARED_LIBRARY}"
    IMPORTED_IMPLIB "${SUNRISE_IMPORT_LIBRARY}"
)
add_dependencies(sunrise zig_build_sunrise)

# --- Install -----------------------------------------------------------------

include(GNUInstallDirs)

install(FILES "${SUNRISE_SHARED_LIBRARY}" TYPE BIN)
install(FILES "${SUNRISE_IMPORT_LIBRARY}" TYPE LIB)
