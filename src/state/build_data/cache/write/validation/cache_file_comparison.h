#pragma once

namespace sunrise::state::build_data::cache::validation {

/**
 * Compares two closed cache files without keeping their contents or allocating.
 * @param firstPath Null-terminated first file path.
 * @param secondPath Null-terminated second file path.
 * @return True when the sizes and every byte match, and both handles close cleanly.
 */
[[nodiscard]] bool files_equal(const wchar_t* firstPath, const wchar_t* secondPath) noexcept;

} // namespace sunrise::state::build_data::cache::validation
