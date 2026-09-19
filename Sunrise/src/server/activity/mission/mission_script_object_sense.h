#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "../../../middleware/bap/activity_message/interactable_object_auth.h"
#include "../../../middleware/bap/activity_message/sense_update.h"

namespace sunrise::server::activity::mission {

/** Type-4 object Sense root ordinals. Ordinal 2 is existence; the client clears it on death. */
inline constexpr std::uint16_t kObjectGenerationOrdinal = 0;
/** Ordinal 1 is the property the mode-2 interaction effect clears. The meaning is assumed. */
inline constexpr std::uint16_t kObjectInteractionOpenOrdinal = 1;
inline constexpr std::uint16_t kObjectExistsOrdinal = 2;
/** Local aliases: ordinal 1 is read as the alive level, ordinal 2 as presence. */
inline constexpr std::uint16_t kObjectAliveOrdinal = kObjectInteractionOpenOrdinal;
inline constexpr std::uint16_t kObjectPresentOrdinal = kObjectExistsOrdinal;
/** Authored entry the object instantiated, and its two 32-bit spawn-mask words. */
inline constexpr std::uint16_t kObjectEntryIndexOrdinal = 3;
inline constexpr std::uint32_t kObjectSpawnMaskSchema = 0x80809E1BU;
/** Ownership reply: field 0 held, field 1 the owner key. */
inline constexpr std::uint16_t kOwnerHeldOrdinal = 0;
inline constexpr std::uint16_t kOwnerKeyOrdinal = 1;
/** Interaction reply: field 0 is the one-shot used latch. */
inline constexpr std::uint16_t kInteractedOrdinal = 0;

/** Last object level seen for one slot. */
struct ObjectInteractionLevel final {
    std::int32_t generation{};
    bool generationKnown{};
    bool interacted{};
    bool interactionKnown{};
    bool present{};
    bool interactionOpen{};
    bool stateKnown{};
    bool ownerKnown{};
    bool hasOwner{};
    std::uint64_t ownerKey{};
    std::int32_t entryIndex{};
    std::array<std::uint32_t, 2> spawnMask{};
};

/**
 * Merges one decoded type-4 body into the retained level. A new generation clears the level, so
 * a stale interaction latch never joins a new object. A body is a full snapshot, so an absent
 * ownership reply clears the owner.
 * @return True when this body is the first report of an accepted interaction for its generation.
 */
[[nodiscard]] inline bool update_object_interaction(
    ObjectInteractionLevel& level,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root) noexcept {
    namespace object = middleware::bap::activity_message::interactable_object;
    const ObjectInteractionLevel before = level;
    for (const auto& value : values) {
        if (!value.present || value.schemaRow != root
            || value.fieldOrdinal != kObjectGenerationOrdinal) {
            continue;
        }
        const auto generation = static_cast<std::int32_t>(value.signedValue);
        if (level.generationKnown && generation < level.generation) {
            return false;
        }
        if (!level.generationKnown || generation != level.generation) {
            level = {};
        }
        level.generation = generation;
        level.generationKnown = true;
    }
    level.ownerKnown = false;
    level.hasOwner = false;
    level.ownerKey = 0;
    for (const auto& value : values) {
        if (!value.present) {
            continue;
        }
        if (value.schemaRow == root && value.fieldOrdinal == kObjectInteractionOpenOrdinal) {
            level.interactionOpen = value.unsignedValue != 0;
        } else if (value.schemaRow == root && value.fieldOrdinal == kObjectExistsOrdinal) {
            level.present = value.unsignedValue != 0;
            level.stateKnown = true;
        } else if (value.schemaRow == root && value.fieldOrdinal == kObjectEntryIndexOrdinal) {
            level.entryIndex = static_cast<std::int32_t>(value.signedValue);
        } else if (value.schemaRow == kObjectSpawnMaskSchema && value.fieldOrdinal == 0
                   && value.occurrence < level.spawnMask.size()) {
            level.spawnMask[value.occurrence] = static_cast<std::uint32_t>(value.unsignedValue);
        } else if (value.schemaRow == object::kOwnershipReply
                   && value.fieldOrdinal == kOwnerHeldOrdinal) {
            level.ownerKnown = true;
            level.hasOwner = value.unsignedValue != 0;
        } else if (value.schemaRow == object::kOwnershipReply
                   && value.fieldOrdinal == kOwnerKeyOrdinal) {
            level.ownerKey = value.unsignedValue;
        } else if (value.schemaRow == object::kInteractionReply
                   && value.fieldOrdinal == kInteractedOrdinal) {
            level.interacted = value.unsignedValue != 0;
            level.interactionKnown = true;
        }
    }
    return level.generationKnown && level.generation > 0 && level.interactionKnown
           && level.interacted
           && (!before.interactionKnown || !before.interacted
               || before.generation != level.generation);
}

} // namespace sunrise::server::activity::mission
