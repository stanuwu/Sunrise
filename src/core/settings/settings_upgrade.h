#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace sunrise::core::settings::upgrade {

/**
 * @param document Borrowed settings file text.
 * @return True when the document was written against an older layout version.
 */
[[nodiscard]] bool needed(std::string_view document) noexcept;

/**
 * Rewrites an older settings document onto the current layout. Members whose value form changed
 * take the bundled default, and every other member is copied through unchanged.
 * @param document Borrowed settings file text.
 * @param bundled Borrowed default document holding the replacement members.
 * @param output Receives the rewritten document.
 * @param written Receives the rewritten length.
 * @return True when the whole rewritten document fits the output.
 */
[[nodiscard]] bool apply(std::string_view document,
                         std::string_view bundled,
                         std::span<char> output,
                         std::size_t& written) noexcept;

} // namespace sunrise::core::settings::upgrade
