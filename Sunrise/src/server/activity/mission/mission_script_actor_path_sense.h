#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

#include "../../../middleware/content/packages/schema/generated/tag_classes_80807f.h"
#include "mission_script_combatant_damage_sense.h"

namespace sunrise::server::activity::mission {

namespace generated = middleware::content::packages::schema::generated;

/** Nested combatant program record: field 0 is the program state, field 1 its revision. */
inline constexpr std::uint32_t kCombatantProgramSchema = generated::C80807f6e::kClass;
/** Root ordinals of the type-2 Sense body. */
inline constexpr std::uint16_t kActorGenerationOrdinal = kCombatantDamageRevisionOrdinal;
inline constexpr std::uint16_t kActorDeliveryRevisionOrdinal = 5;
inline constexpr std::uint16_t kActorDeliveryStateOrdinal = 6;
inline constexpr std::uint16_t kActorSuppressedOrdinal = kCombatantSuppressedOrdinal;
inline constexpr std::uint16_t kActorInitializedOrdinal = kCombatantFirstAuthOrdinal;
/** Which fields a level has seen within its current initialized lifetime. */
inline constexpr std::uint8_t kActorSeenGeneration = 0x01;
inline constexpr std::uint8_t kActorSeenRevision = 0x02;
inline constexpr std::uint8_t kActorSeenState = 0x04;
inline constexpr std::uint8_t kActorSeenSuppressed = 0x08;
inline constexpr std::uint8_t kActorSeenDeliveryRevision = 0x10;
inline constexpr std::uint8_t kActorSeenDeliveryState = 0x20;
inline constexpr std::uint8_t kActorSeenInitialized = 0x40;
/** Only an initialized complete program level can produce an actor-path event. */
inline constexpr std::uint8_t kActorSeenCore = kActorSeenGeneration | kActorSeenRevision
                                               | kActorSeenState | kActorSeenSuppressed
                                               | kActorSeenInitialized;
inline constexpr std::uint8_t kActorSeenDelivery =
    kActorSeenDeliveryRevision | kActorSeenDeliveryState;

/** Suppression also describes detached or uncreated actors; it is never a death receipt. */
struct ActorPathLevel final {
    std::int32_t generation{};
    std::int32_t revision{};
    std::int32_t state{};
    std::int32_t deliveryRevision{};
    std::int32_t deliveryState{};
    std::uint8_t seen{};
    bool suppressed{};
    bool initialized{};
    bool operator==(const ActorPathLevel&) const = default;
};

/** Reads a reflected integer without narrowing an invalid value or accepting another scalar kind.
 */
[[nodiscard]] inline bool
read_actor_integer(const middleware::bap::activity_message::sense_update::DecodedValue& value,
                   std::int32_t& output) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    if (value.kind == sense::ValueKind::unsignedInteger
        && value.unsignedValue <= (std::numeric_limits<std::int32_t>::max)()) {
        output = static_cast<std::int32_t>(value.unsignedValue);
        return true;
    }
    if (value.kind == sense::ValueKind::signedInteger
        && value.signedValue >= (std::numeric_limits<std::int32_t>::min)()
        && value.signedValue <= (std::numeric_limits<std::int32_t>::max)()) {
        output = static_cast<std::int32_t>(value.signedValue);
        return true;
    }
    return false;
}

/**
 * Retains sparse program fields only within the same initialized spawn and program revisions.
 * @param level Last accepted level for this exact authored actor.
 * @param values Accepted reflected fields of one Type-2 report.
 * @param root Schema row of the body's root.
 * @return True when an initialized complete level changed; invalid input changes nothing.
 */
[[nodiscard]] inline bool update_actor_path_level(
    ActorPathLevel& level,
    std::span<const middleware::bap::activity_message::sense_update::DecodedValue> values,
    std::uint32_t root) noexcept {
    namespace sense = middleware::bap::activity_message::sense_update;
    ActorPathLevel incoming{};
    for (const auto& value : values) {
        if (!value.present || value.occurrence != 0) {
            continue;
        }
        std::int32_t* integer = nullptr;
        std::uint8_t field = 0;
        if (value.schemaRow == kCombatantProgramSchema) {
            if (value.fieldOrdinal == 0) {
                integer = &incoming.state;
                field = kActorSeenState;
            } else if (value.fieldOrdinal == 1) {
                integer = &incoming.revision;
                field = kActorSeenRevision;
            }
        } else if (value.schemaRow == root) {
            switch (value.fieldOrdinal) {
            case kActorGenerationOrdinal:
                integer = &incoming.generation;
                field = kActorSeenGeneration;
                break;
            case kActorDeliveryRevisionOrdinal:
                integer = &incoming.deliveryRevision;
                field = kActorSeenDeliveryRevision;
                break;
            case kActorDeliveryStateOrdinal:
                integer = &incoming.deliveryState;
                field = kActorSeenDeliveryState;
                break;
            case kActorSuppressedOrdinal:
            case kActorInitializedOrdinal:
                field = value.fieldOrdinal == kActorSuppressedOrdinal ? kActorSeenSuppressed
                                                                      : kActorSeenInitialized;
                if (value.kind != sense::ValueKind::boolean || value.unsignedValue > 1) {
                    return false;
                }
                if (field == kActorSeenSuppressed) {
                    incoming.suppressed = value.unsignedValue != 0;
                } else {
                    incoming.initialized = value.unsignedValue != 0;
                }
                break;
            default:
                break;
            }
        }
        if (field == 0) {
            continue;
        }
        if ((incoming.seen & field) != 0
            || (integer != nullptr && !read_actor_integer(value, *integer))) {
            return false;
        }
        incoming.seen |= field;
    }
    if (incoming.seen == 0) {
        return false;
    }
    if ((incoming.seen & kActorSeenGeneration) != 0 && incoming.generation < level.generation) {
        return false;
    }
    if ((incoming.seen & kActorSeenInitialized) != 0 && !incoming.initialized) {
        const auto highWater = (std::max)(level.generation, incoming.generation);
        level = {};
        level.generation = highWater;
        level.seen = kActorSeenInitialized;
        return false;
    }
    const bool newSpawn =
        (incoming.seen & kActorSeenGeneration) != 0
        && ((level.seen & kActorSeenGeneration) == 0 || incoming.generation != level.generation);
    if (!newSpawn
        && (((incoming.seen & level.seen & kActorSeenRevision) != 0
             && incoming.revision < level.revision)
            || ((incoming.seen & level.seen & kActorSeenDeliveryRevision) != 0
                && incoming.deliveryRevision < level.deliveryRevision))) {
        return false;
    }
    ActorPathLevel next = newSpawn ? ActorPathLevel{} : level;
    if ((incoming.seen & kActorSeenRevision) != 0
        && ((next.seen & kActorSeenRevision) == 0 || incoming.revision != next.revision)) {
        next.seen &= static_cast<std::uint8_t>(~kActorSeenState);
    }
    if ((incoming.seen & kActorSeenDeliveryRevision) != 0
        && ((next.seen & kActorSeenDeliveryRevision) == 0
            || incoming.deliveryRevision != next.deliveryRevision)) {
        next.seen &= static_cast<std::uint8_t>(~kActorSeenDeliveryState);
    }
    if ((incoming.seen & kActorSeenGeneration) != 0) {
        next.generation = incoming.generation;
    }
    if ((incoming.seen & kActorSeenRevision) != 0) {
        next.revision = incoming.revision;
    }
    if ((incoming.seen & kActorSeenState) != 0) {
        next.state = incoming.state;
    }
    if ((incoming.seen & kActorSeenDeliveryRevision) != 0) {
        next.deliveryRevision = incoming.deliveryRevision;
    }
    if ((incoming.seen & kActorSeenDeliveryState) != 0) {
        next.deliveryState = incoming.deliveryState;
    }
    if ((incoming.seen & kActorSeenSuppressed) != 0) {
        next.suppressed = incoming.suppressed;
    }
    if ((incoming.seen & kActorSeenInitialized) != 0) {
        next.initialized = incoming.initialized;
    }
    next.seen |= incoming.seen;
    const bool changed = next != level;
    level = next;
    return changed && next.initialized && (next.seen & kActorSeenCore) == kActorSeenCore;
}

} // namespace sunrise::server::activity::mission
