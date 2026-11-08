#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../defaults/definition.h"
#include "../destination/definition.h"

namespace sunrise::state::activity::forced {

/** The bubble number picks one of the 64 wire slots. */
inline constexpr std::uint8_t kMaximumBubble = 63;
static_assert(kMaximumBubble + std::size_t{1} == defaults::kBubbleCapacity);
/** The slice-set index is a 10-bit bias-1 wire field, so this is the largest it can hold. */
inline constexpr std::uint16_t kMaximumSliceSet = 1'022;
/**
 * The hash that names no set, so the Client searches the loaded world itself.
 * A set belongs to the map, not the bubble. Send one the bubble lacks and nothing spawns.
 */
inline constexpr std::uint32_t kAbsentSpawnSetHash = 0x811C9DC5U;

/**
 * One operator-chosen destination that replaces what the client asked for.
 * Nothing here is saved. The process starts with no selection and the switch off.
 */
struct ForcedDestination {
    /** Destination package name, without its `:scenario_client` suffix. */
    std::array<char, destination::kPackageNameCapacity> packageName{};
    std::uint8_t packageNameLength{};
    std::uint8_t bubble{};
    std::uint16_t sliceSet{};
    /** Spawn-set name hash, used only when one was chosen. */
    std::uint32_t spawnSetHash{};
    /**
     * Investment activity index of the definition to bind, when one was chosen.
     * A package name maps to several definitions, so the SDK and the mission script cannot
     * resolve one from the name. Naming it here is what lets them bind under an override.
     */
    std::uint16_t activityIndex{};
    bool hasBubble{};
    bool hasSliceSet{};
    bool hasSpawnSetHash{};
    bool hasActivityIndex{};
    /** The global switch. Off means the client's own selection stands. */
    bool enabled{};
};

/**
 * Tests whether a forced destination names enough to replace a client selection.
 * The spawn set is the one optional part. Without it the Client picks its own point.
 * @param value Candidate selection.
 * @return True when the switch is on and the destination, bubble, and slice set are all named.
 */
[[nodiscard]] constexpr bool active(const ForcedDestination& value) noexcept {
    return value.enabled && value.packageNameLength != 0
           && value.packageNameLength <= value.packageName.size() && value.hasBubble
           && value.bubble <= kMaximumBubble && value.hasSliceSet
           && value.sliceSet <= kMaximumSliceSet;
}

/**
 * Tests whether a candidate can be stored, complete or not.
 * A partial selection is kept so the interface can set one field at a time.
 * @param value Candidate selection.
 * @return True when every named field is inside its wire range.
 */
[[nodiscard]] constexpr bool storable(const ForcedDestination& value) noexcept {
    return value.packageNameLength <= value.packageName.size()
           && (!value.hasBubble || value.bubble <= kMaximumBubble)
           && (!value.hasSliceSet || value.sliceSet <= kMaximumSliceSet)
           && (!value.hasActivityIndex
               || value.activityIndex
                      <= static_cast<std::uint16_t>(destination::kMaximumActivityIndex));
}

} // namespace sunrise::state::activity::forced
