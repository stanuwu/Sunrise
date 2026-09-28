#pragma once

#include "definition.h"

namespace sunrise::state::build_data::rewards {

/** Discards the reward graph and every borrowed bank. */
void clear() noexcept;
/** True once pool and item banks have been published. */
[[nodiscard]] bool ready() noexcept;
/** Checks bank ranges, item references and bounded acyclic pool traversal. */
[[nodiscard]] bool valid(View data) noexcept;
/** Publishes validated banks together under the catalog lock. */
[[nodiscard]] bool replace(View data) noexcept;
/** The borrowed view remains valid only during the callback. */
[[nodiscard]] bool read(void* context, bool (*consume)(void*, View) noexcept) noexcept;
/** Copies a native item row, refusing unavailable extraction entries. */
[[nodiscard]] bool find_item(std::uint16_t itemIndex, Item& item) noexcept;

/** Copies one complete bank; count is zero if the output is too small. */
[[nodiscard]] bool snapshot(std::span<Pool> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Entry> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Item> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Instruction> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<Modifier> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<SocketOverride> output, std::size_t& count) noexcept;

} // namespace sunrise::state::build_data::rewards
