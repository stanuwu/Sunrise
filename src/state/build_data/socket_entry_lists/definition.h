#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::socket_entry_lists {

/** Signed native definition indices give 32,768 nonnegative socket-list rows. */
inline constexpr std::size_t kDefinitionCapacity = 32768;
/** One item-instance record has 36 socket-entry state lanes. */
inline constexpr std::size_t kEntryCapacity = 36;
/** Native state 0 marks a socket entry whose plug source is absent. */
inline constexpr std::uint8_t kAbsentEntryState = 0;
/** Native state 16 marks an initial socket entry whose plug source is available. */
inline constexpr std::uint8_t kReadyEntryState = 16;

/** Native state 18 marks a socket entry the character has selected. */
inline constexpr std::uint8_t kActiveEntryState = 18;
/** Entry kind of the super lane, which is active although it carries no plug source. */
inline constexpr std::uint8_t kSuperEntryKind = 34;
/** Group value of an entry that competes in no bucket. */
inline constexpr std::uint8_t kNoEntryGroup = 0xFF;
/** Plug source of an entry that has none. */
inline constexpr std::uint32_t kNoPlugSource = 0x811C9DC5U;

/** One socket entry's competition group, kind and plug source. */
struct Entry {
    std::uint32_t plugSource{};
    std::uint8_t group{kNoEntryGroup};
    std::uint8_t kind{};
};

/** One installed-build socket-entry-list identity and its safe initial state mask. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint16_t definitionIndex{};
    std::uint8_t entryCount{};
    std::uint64_t readyMask{};
};

/** List index of an unused entry-table row. */
inline constexpr std::uint16_t kNoEntryTable = 0xFFFF;

/**
 * Per-entry selection inputs, kept only for lists that carry a super lane. Those are the
 * subclasses, the only items whose sockets a character selects. Holding this on every definition
 * would cost 10 MB of fixed storage to serve 9 items.
 */
struct EntryTable {
    std::uint16_t definitionIndex{kNoEntryTable};
    std::array<Entry, kEntryCapacity> entries{};
};

/** Lists that may carry a super lane. Only 9 subclasses ship, so this is plenty. */
inline constexpr std::size_t kEntryTableCapacity = 32;

} // namespace sunrise::state::build_data::socket_entry_lists
