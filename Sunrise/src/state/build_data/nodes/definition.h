#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::nodes {

/** The shipped build declares 924 presentation nodes. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 1024;

/** No shipped node owns more records than this; the widest seen is a lore book at fifteen. */
inline constexpr std::size_t kChildCapacity = 64;

/** A node whose expression names no addressable value slot carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableValueIndex = 0xFFFFU;

/**
 * The lore book categories: 815 to 829 under The Light, 831 to 843 under The Darkness, and 845 to
 * 854 under Dusk and Dawn.
 *
 * The upper bound was 853, counted from the game's own list, and it was one short: node 854 carries
 * the same gate every other book carries, a value read tested against zero, while 855 onward pair
 * two flag reads or use a different opcode entirely. The end of the range is therefore taken from
 * the shape of the rows rather than from counting entries on screen. Only these nodes have their
 * visibility gate satisfied.
 */
inline constexpr std::uint16_t kLoreNodeFirst = 815U;
inline constexpr std::uint16_t kLoreNodeLast = 854U;

/** @return True when this node is a lore book category, the only kind this build counts. */
[[nodiscard]] constexpr bool lore_category(std::uint16_t definitionIndex) noexcept {
    return definitionIndex >= kLoreNodeFirst && definitionIndex <= kLoreNodeLast;
}

/** A node whose gate names no addressable flag carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableFlagIndex = 0xFFFFU;

/**
 * One presentation node reduced to what a progress bar needs.
 *
 * A node's bar is not derived by the client from its children. The node carries an expression that
 * reads a value slot, and the bar shows whatever that slot holds, so a server that wants the bar to
 * move has to count the claimed children itself and write the count. The slot is resolved to its
 * mapping-table row here, at extraction, exactly as a record's completion flag is.
 */
struct Definition {
    /** Native node row. */
    std::uint16_t definitionIndex{};
    /** Account value bank mapping row, or kUnavailableValueIndex when no slot is addressable. */
    std::uint16_t valueIndex{kUnavailableValueIndex};
    /**
     * Account value index of the parent record's own bar, one slot above the node's.
     *
     * The node's bar counts every child including the parent record; the parent's bar counts only
     * the chapters. They are separate slots and need separate counts.
     */
    std::uint16_t parentValueIndex{kUnavailableValueIndex};
    /**
     * Account flag index this node's own gate reads, or kUnavailableFlagIndex.
     *
     * A category with a redacted title is not waiting on progress: its expression reads a flag, and
     * until that flag is set the client shows no name and offers nothing inside to claim. A book
     * gated this way cannot reveal itself by being played, so the gate is satisfied here.
     */
    std::uint16_t visibilityFlagIndex{kUnavailableFlagIndex};
    /**
     * Character flag index this node's gate reads, or kUnavailableFlagIndex.
     *
     * Not every gate is account scoped. One lore book names a slot that no account mapping row
     * carries at all, and the same slot sits in the character table instead, so a gate is resolved
     * against both banks and satisfied in whichever one claims it.
     */
    std::uint16_t visibilityCharacterFlagIndex{kUnavailableFlagIndex};
    /**
     * Character value index this node's bar reads, or kUnavailableValueIndex.
     *
     * Not every category counts in the account bank. One lore book reads a slot that only the
     * character table carries, so it stayed redacted while every other book opened: its count had
     * nowhere to go.
     */
    std::uint16_t characterValueIndex{kUnavailableValueIndex};
    /** Records this node owns, held at node row `+136`. */
    std::uint8_t childCount{};
    /** Native record rows of the owned records. */
    std::array<std::uint16_t, kChildCapacity> children{};
};

} // namespace sunrise::state::build_data::nodes
