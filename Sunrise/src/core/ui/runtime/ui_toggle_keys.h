#pragma once

#include <Windows.h>

#include <string_view>

namespace sunrise::core::ui::runtime {

/**
 * Maps one settings key name to a Windows virtual key from the supported toggle set.
 * @param name Lower-case name the settings file uses.
 * @param output Receives the virtual key; unchanged when the name is not supported.
 * @return True when the name is in the supported set.
 */
[[nodiscard]] bool toggle_key_value(std::string_view name, UINT& output) noexcept;

/** @return The settings name of a supported toggle key, or empty when it is not one. */
[[nodiscard]] std::string_view toggle_key_name(UINT virtualKey) noexcept;

} // namespace sunrise::core::ui::runtime
