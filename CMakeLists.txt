cmake_minimum_required(VERSION 3.31)
project(Sunrise C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(XWIN_SDK_VERSION "10.0.26100" CACHE STRING "xwin SDK Version")

file(GLOB_RECURSE SUNRISE_SOURCES CONFIGURE_DEPENDS
    "Sunrise/src/*.cpp"
    "Sunrise/src/*.h"
)
file(GLOB_RECURSE VENDOR_SOURCES CONFIGURE_DEPENDS
    "Sunrise/vendor/detours/*.cpp"
    "Sunrise/vendor/detours/*.h"
    "Sunrise/vendor/imgui/*.cpp"
    "Sunrise/vendor/imgui/*.h"
    "Sunrise/vendor/lua/*.c"
    "Sunrise/vendor/lua/*.h"
    "Sunrise/vendor/sqlite/*.c"
    "Sunrise/vendor/sqlite/*.h"
)
# Lua builds as C++ here, matching the MSVC project's CompileAsCpp setting.
file(GLOB_RECURSE LUA_SOURCES CONFIGURE_DEPENDS
    "Sunrise/vendor/lua/*.c"
)
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
file(GLOB_RECURSE RESOURCE_FILES CONFIGURE_DEPENDS
    "Sunrise/resources/*.rc"
    "Sunrise/resources/*.h"
    "Sunrise/resources/*.json"
    "Sunrise/resources/*.sql"
)

add_library(steam_api64 SHARED
    ${SUNRISE_SOURCES}
    ${VENDOR_SOURCES}
    ${RESOURCE_FILES}
)

set_property(TARGET steam_api64 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported)
if(lto_supported)
    set_property(TARGET steam_api64 APPEND PROPERTY
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
endif()
target_include_directories(steam_api64
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/src"
)

# sunrise.rc names its payloads relative to itself, so its directory has to be on the include path.
target_include_directories(steam_api64
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/resources"
)

target_include_directories(steam_api64 SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/vendor/detours"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/vendor/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/vendor/imgui/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/vendor/lua"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sunrise/vendor/sqlite"
)

if(DEFINED XWIN_DIR)
    target_include_directories(steam_api64 SYSTEM PRIVATE
        "${XWIN_DIR}/sdk/Include/${XWIN_SDK_VERSION}/um"
        "${XWIN_DIR}/sdk/Include/${XWIN_SDK_VERSION}/shared"
    )
endif()

target_compile_definitions(steam_api64 PRIVATE
    WIN32
    _WINDOWS
    _USRDLL
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WINSOCK_DEPRECATED_NO_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
    IMGUI_USER_CONFIG="core/ui/imgui_user_config.h"
)

if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # The vendored Lua interpreter throws, so its error path needs C++ unwinding.
    target_compile_options(steam_api64 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)

    # Clang warnings are more strict the MSVC I guess, this only works on real clang
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND STRICT_WARNING_FLAGS
            -Wno-unused-const-variable
            -Wno-unused-function
            -Wno-braced-scalar-init
            -Wno-missing-braces
            -Wno-missing-designated-field-initializers
            -Wno-cast-function-type-mismatch
        )
    endif()

    set_source_files_properties(${SUNRISE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${STRICT_WARNING_FLAGS}"
    )
    set_source_files_properties(${VENDOR_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/W3"
    )

    # The game's window procedure can throw through CallWindowProcW; /EHc- turns /EHsc into /EHs.
    set_property(SOURCE
        "Sunrise/src/client/hooks/graphics/input/graphics_window_input.cpp"
        "Sunrise/src/client/hooks/graphics/input/graphics_raw_input_window.cpp"
        APPEND PROPERTY COMPILE_OPTIONS "/EHc-"
    )
endif()

if(CMAKE_HOST_LINUX AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(steam_api64 PRIVATE
        $<$<CONFIG:Debug>:/clang:-gdwarf>
        $<$<CONFIG:Debug>:/clang:-g>
    )

    target_link_options(steam_api64 PRIVATE
        $<$<CONFIG:Debug>:/DEBUG:DWARF>
    )
endif()

target_link_libraries(steam_api64 PRIVATE
    kernel32 bcrypt user32 gdi32 dwmapi
    d3dcompiler shell32 ws2_32 synchronization
    windowscodecs
)

set_target_properties(steam_api64 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/x64/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/x64/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/x64/$<CONFIG>"
)

set_source_files_properties(Sunrise/vendor/sqlite/sqlite3.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_OMIT_LOAD_EXTENSION;SQLITE_THREADSAFE=1"
)
