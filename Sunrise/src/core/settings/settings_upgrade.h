#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sunrise::core::settings::upgrade {

/** Oldest file version that upgrades in place; an older file is replaced by the defaults. */
inline constexpr std::uint32_t kOldestUpgradable = 18;

/**
 * Rewrites settings text from `version` to the build's version, one step per version.
 * Keeps every value and the file's layout; only moved or renamed keys change.
 * @param document Complete settings text at `version`.
 * @param version Version the text declares.
 * @param output Receives the rewritten text.
 * @return False when no step covers `version` or the text does not have the expected shape.
 */
[[nodiscard]] bool
to_current(std::string_view document, std::uint32_t version, std::string& output) noexcept;

} // namespace sunrise::core::settings::upgrade
