#pragma once

#include <cstddef>
#include <span>

namespace sunrise::core::ui::components::picker {

/** Longest filter text the popup keeps, which is more than any row label needs. */
inline constexpr std::size_t kFilterCapacity = 64;

/** One row of a picker list. The label is borrowed for the duration of the call. */
struct Item {
    const char* label{};
};

/**
 * Draws a dropdown whose popup filters its rows as you type.
 * The filter is a case-insensitive substring match over the row labels, so callers pass rows and
 * nothing else. Selection stays with the caller: this reports a pick, it does not store one.
 * @param id Stable Dear ImGui scope ID.
 * @param preview Text shown while the popup is closed.
 * @param items Rows, valid for this call.
 * @param selected Receives the index of the picked row.
 * @return True when a row was picked this frame.
 */
[[nodiscard]] bool control(const char* id,
                           const char* preview,
                           std::span<const Item> items,
                           std::size_t& selected) noexcept;

} // namespace sunrise::core::ui::components::picker
