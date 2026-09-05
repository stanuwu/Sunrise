cmake_minimum_required(VERSION 3.20)
project(SunriseSettingsUpgradeTests LANGUAGES CXX)

enable_testing()
add_executable(settings_upgrade_test
    settings_upgrade_test.cpp
    ../../Sunrise/src/core/settings/settings_upgrade.cpp
)
target_compile_features(settings_upgrade_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(settings_upgrade_test PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(settings_upgrade_test PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME settings_upgrade COMMAND settings_upgrade_test)
