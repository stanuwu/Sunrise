#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::scenarios {

/**
 * Destinations the class sweep finds in the installed packages. The live count is 468 and the
 * reference name table holds 460, so this leaves room above both.
 */
inline constexpr std::size_t kDefinitionCapacity = 512;
/**
 * Activity message 1 carries one byte per bubble and no installed scenario declares more. The
 * region space is 512 wide with bubbles spaced by the slice-set factor, so this is what fits it.
 */
inline constexpr std::size_t kBubbleCapacity = 64;
/**
 * A destination is looked up by the package name in the client's selection. That field is 40
 * bytes wide, so a longer scenario name could never match.
 */
inline constexpr std::size_t kNameCapacity = 40;
/** The byte a bubble carries when its first slice-set state is enabled. */
inline constexpr std::uint8_t kBubbleEnabledByte = 0x80;
/** The byte every other bubble carries, including one with no readable state array. */
inline constexpr std::uint8_t kBubbleDisabledByte = 0x7F;
/**
 * Roster group objects the installed packages declare. The live count is 68.
 * A group object is one whose slot list declares a type activity message 5 publishes.
 */
inline constexpr std::size_t kRosterGroupCapacity = 128;
/** Slots on one roster group object. The widest installed group declares 1218. */
inline constexpr std::size_t kRosterSlotCapacity = 1280;
/** Roster groups one destination publishes. No installed destination reaches more than two. */
inline constexpr std::size_t kDestinationGroupCapacity = 4;
/** Slot flag bit for a slot whose type declares a sense schema. */
inline constexpr std::uint8_t kSlotSenseFlag = 1;
/** Slot flag bit for a slot whose type declares an auth schema. */
inline constexpr std::uint8_t kSlotAuthFlag = 2;
/** Both flag bits together, which is the widest value a slot flag byte may hold. */
inline constexpr std::uint8_t kSlotFlagMask = kSlotSenseFlag | kSlotAuthFlag;
/** The widest slot type the installed packages declare. */
inline constexpr std::uint8_t kMaximumSlotType = 72;
/**
 * Storage for a destination's map-package stem, which is the key its spawn sets are grouped under.
 * The longest installed stem is `mercury_destination` at 19 bytes.
 */
inline constexpr std::size_t kSpawnStemCapacity = 32;
/** Packages one destination's slice-set entries name. The widest installed one names three. */
inline constexpr std::size_t kDestinationPackageCapacity = 8;

/**
 * One roster group object and the slots activity message 5 seeds for it.
 * Slot indices are contiguous from zero, so a slot's ordinal in these arrays is its index.
 */
struct RosterGroup {
    std::uint32_t registryKey{};
    /** Tag the slots were read from, kept so a stale cache can be told from a missing one. */
    std::uint32_t objectTag{};
    std::uint16_t slotCount{};
    std::array<std::uint8_t, kRosterSlotCapacity> slotTypes{};
    std::array<std::uint8_t, kRosterSlotCapacity> slotFlags{};
};

/** One destination's bubble layout, reduced to what the activity messages publish. */
struct Definition {
    /** Lowercase package name without its `:scenario_client` suffix. */
    std::array<char, kNameCapacity> name{};
    /** Tag the layout was read from, kept so a stale cache can be told from a missing one. */
    std::uint32_t tag{};
    std::uint8_t nameLength{};
    /** Used entries in the arrays below. */
    std::uint8_t bubbleCount{};
    /** Set when the scenario declared more bubbles than the wire array holds. */
    std::uint8_t truncated{};
    /**
     * Groups this destination reaches in every one of its slice sets, as roster table indices.
     * A key the current slice set cannot find crashes the client's teardown sweep, so a group
     * present in only some of them is dropped, not published.
     */
    std::uint8_t rosterGroupCount{};
    /**
     * Map-package stem this destination loads from, or empty when the sweep did not name it.
     * It is the key the spawn-set catalog is grouped under.
     * Spawn-set catalog: `state/build_data/spawn_sets`.
     */
    std::uint8_t spawnStemLength{};
    std::array<char, kSpawnStemCapacity> spawnStem{};
    std::array<std::uint16_t, kDestinationGroupCapacity> rosterGroups{};
    /**
     * One wire byte per bubble, as activity message 1 publishes them. Every byte comes from the
     * scenario blob alone, which is why one tag read per destination builds the whole domain.
     */
    std::array<std::uint8_t, kBubbleCapacity> bubbleStates{};
    /**
     * Each bubble's own name hash, in the same order as the states.
     * The client names its arrival bubble by hash. This turns that hash into the index whose
     * slice set the host publishes.
     */
    std::array<std::uint32_t, kBubbleCapacity> bubbleHashes{};
    /**
     * Slice-set states each bubble declares, in the same order as the states.
     * A bubble's slice sets run from its region index for this many entries, so a bubble with two
     * states owns two of them.
     */
    std::array<std::uint8_t, kBubbleCapacity> bubbleStateCounts{};
    /**
     * Each bubble's map-global index, in the same order as the states.
     * Bubbles are numbered per map, not per destination, and a spawn set names its bubbles by that
     * number. This is what narrows the spawn-set list to one bubble.
     */
    std::array<std::uint16_t, kBubbleCapacity> bubbleMapIndices{};
    /**
     * Packages this destination loads, from its slice-set entries and its own tag.
     * A spawn set that lives in an activity package outside this list is one this destination does
     * not stream, however well the bubble mask fits.
     */
    std::uint8_t packageCount{};
    std::array<std::uint16_t, kDestinationPackageCapacity> packages{};
};

} // namespace sunrise::state::build_data::scenarios
