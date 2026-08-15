#pragma once

#include <cstddef>
#include <cstdint>

#include "../destination/definition.h"

namespace sunrise::state::activity::defaults {

/** Global activity state reserves 64 one-byte bubble-state entries. */
inline constexpr std::size_t kBubbleCapacity = 64;
/** Each bubble owns 8 consecutive slice-state indices. */
inline constexpr std::size_t kSliceStatesPerBubble = 8;
/** One bubble is needed for a usable locally-authored destination policy. */
inline constexpr std::uint8_t kMinimumBubbleCount = 1;
/** 64 bubbles with 8 states each give a largest valid index of 511. */
inline constexpr std::uint16_t kMaximumInitialSliceSet = 511;

/** Small numeric launch policy paired with the locally-authored default destination. */
struct FallbackPolicy final {
    /** Number of meaningful entries in the fixed bubble-state array. */
    std::uint8_t bubbleCount{};
    /** One bit per bubble; set bits mark entries that publish slice-state zero. */
    std::uint64_t statefulBubbleMask{};
    /** First slice-state index, picked when no earlier source finds one. */
    std::uint16_t initialSliceSet{};
    /** Spawn-set name hash used by the initial slice-state selection. */
    std::uint32_t spawnSetHash{};
};

/** One whole absent-selection fallback without a package-content map. */
struct DefaultDestination final {
    destination::DestinationSelection selection{};
    FallbackPolicy fallback{};
};

/**
 * Destinations that may carry an authored arrival override.
 * A few maps bind their arrival when the map loads instead of declaring it in the packages, so no
 * walk can derive those. The reference set is 20 rows. This leaves room above it.
 */
inline constexpr std::size_t kArrivalOverrideCapacity = 64;

/**
 * One authored arrival for a named destination, applied over every derived source.
 * Neither field is needed. A row may move only the bubble, only the spawn set, or both.
 */
struct ArrivalOverride final {
    std::array<char, destination::kPackageNameCapacity> name{};
    std::uint8_t nameLength{};
    std::uint8_t bubble{};
    bool hasBubble{};
    std::uint32_t spawnSetHash{};
    bool hasSpawnSetHash{};
};

/** Immutable activity defaults supplied while the root State is initialized. */
struct ActivityDefaults final {
    DefaultDestination defaultDestination{};
    std::array<ArrivalOverride, kArrivalOverrideCapacity> arrivalOverrides{};
    std::uint8_t arrivalOverrideCount{};
    /**
     * Sends the membership identity's `field3` as message 5's player key, not the character SOID.
     * That field is the member record's `+16`, which is the value this key must equal.
     */
    bool rosterKeyFromIdentity{};
    /**
     * Fills message 5's participation body on every type-13 slot of the key group.
     * The old encoder fills only the group's first, and the gate reads whichever object the player
     * datum names, which need not be that one.
     */
    bool rosterKeyOnAllSlots{};
};

} // namespace sunrise::state::activity::defaults
