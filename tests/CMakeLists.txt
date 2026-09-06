cmake_minimum_required(VERSION 3.20)
project(SunrisePortableTests LANGUAGES CXX)
enable_testing()

# Assertions must remain enabled in optimized test builds.
if(MSVC)
    string(REGEX REPLACE "(^| )/D[ ]*NDEBUG( |$)" " " CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()
function(sunrise_test name)
    cmake_parse_arguments(TEST "" "" "ARGS" ${ARGN})
    add_executable(${name}_test ${name}_test.cpp ${TEST_UNPARSED_ARGUMENTS})
    target_compile_features(${name}_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${name}_test PRIVATE /W4 /WX /UNDEBUG)
    else()
        target_compile_options(${name}_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name}_test ${TEST_ARGS})
    set_tests_properties(${name} PROPERTIES TIMEOUT 20)
endfunction()

sunrise_test(activity_host_selection)
sunrise_test(socket_row_relocation ARGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/socket_rows.hex")
sunrise_test(lore_visibility)
sunrise_test(workspace_state ../Sunrise/src/core/ui/layout/workspace_state.cpp)

if(WIN32)
    sunrise_test(workspace_store
        ../Sunrise/src/core/ui/layout/workspace_state.cpp
        ../Sunrise/src/core/ui/layout/workspace_store.cpp)
    target_compile_definitions(workspace_store_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(WIN32)
    add_executable(workspace_ui_test workspace_ui_test.cpp
        ../Sunrise/src/core/ui/layout/workspace_state.cpp
        ../Sunrise/src/core/ui/modules/ui_module_descriptor.cpp
        ../Sunrise/src/core/ui/modules/registry/ui_module_registry.cpp
        ../Sunrise/src/core/ui/animation/transition/ui_transition_animation.cpp
        ../Sunrise/src/core/ui/components/card/ui_card_component.cpp
        ../Sunrise/src/core/ui/components/logo/ui_logo_component.cpp
        ../Sunrise/src/core/ui/components/navigation/ui_navigation_component.cpp
        ../Sunrise/src/core/ui/components/section/ui_section_component.cpp
        ../Sunrise/src/core/ui/layout/navigation/ui_layout_navigation.cpp
        ../Sunrise/src/core/ui/layout/credits/sunrise_credits_badge.cpp
        ../Sunrise/src/core/ui/theme/sunrise_ui_theme.cpp
        ../Sunrise/src/core/ui/textures/ui_texture_slots.cpp
        ../Sunrise/vendor/imgui/imgui.cpp
        ../Sunrise/vendor/imgui/imgui_draw.cpp
        ../Sunrise/vendor/imgui/imgui_tables.cpp
        ../Sunrise/vendor/imgui/imgui_widgets.cpp
        ../Sunrise/vendor/imgui/backends/imgui_impl_dx11.cpp)
    target_link_libraries(workspace_ui_test PRIVATE d3d11 d3dcompiler shell32 windowscodecs ole32)
    target_compile_features(workspace_ui_test PRIVATE cxx_std_20)
    target_include_directories(workspace_ui_test PRIVATE ../Sunrise/vendor/imgui ../Sunrise/src)
    target_compile_definitions(workspace_ui_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN IMGUI_ENABLE_TEST_ENGINE)
    target_compile_options(workspace_ui_test PRIVATE /UNDEBUG)
    add_test(NAME workspace_ui COMMAND workspace_ui_test)
endif()

if(WIN32)
    sunrise_test(squad_graph_edges
        ../Sunrise/src/client/content/activity/activity_sdk_squad_graph_edges.cpp
        ../Sunrise/src/middleware/crypto/sha256.cpp)
    target_compile_definitions(squad_graph_edges_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(squad_graph_edges_test PRIVATE bcrypt)
    set_tests_properties(workspace_store workspace_ui PROPERTIES TIMEOUT 20)
endif()
