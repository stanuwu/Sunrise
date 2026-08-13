cmake_minimum_required(VERSION 3.20)
project(Sunrise CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Static release CRT (/MT): xwin has no debug CRT.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

file(GLOB_RECURSE SUNRISE_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

file(GLOB DETOURS_SRC CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/vendor/detours/*.cpp")

set(IMGUI_SRC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_win32.cpp
    vendor/imgui/backends/imgui_impl_dx11.cpp)

add_library(steam_api64 SHARED ${SUNRISE_SRC} ${DETOURS_SRC} ${IMGUI_SRC})

set_target_properties(steam_api64 PROPERTIES
    OUTPUT_NAME "steam_api64"
    PREFIX ""
    SUFFIX ".dll")

target_include_directories(steam_api64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/detours
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends)

# UNICODE/_UNICODE: else RT_RCDATA won't match FindResourceW.
target_compile_definitions(steam_api64 PRIVATE
    WIN32 _WINDOWS _USRDLL
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    NDEBUG
    IMGUI_USER_CONFIG="core/ui/imgui_user_config.h")

target_link_libraries(steam_api64 PRIVATE
    kernel32 user32 gdi32 shell32 dwmapi
    bcrypt ws2_32 d3dcompiler synchronization)
