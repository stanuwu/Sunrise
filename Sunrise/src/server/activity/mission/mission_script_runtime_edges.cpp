#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../state/activity_sdk/scene_triggers.h"
#include "../../gameplay/squad_entity_retirement.h"
#include "../activity_sdk_mission_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_runtime_objective.h"
#include "mission_script_slot_index.h"

// Sense events retain levels within the owning mission attempt.

namespace sunrise::server::activity::mission {
namespace {

/** Root ordinal of the squad removal flag. The rest of the root is inert on this build. */
constexpr std::uint16_t kSquadRemovalOrdinal = 8;
/** Root ordinal 0 echoes the spawn generation the squad's placement Auth carries. */
constexpr std::uint16_t kSquadSpawnGenerationOrdinal = 0;
/** Root ordinals of the type-20 damage Sense body: health, shield, then the echoed revision. */
constexpr std::uint16_t kDamageHealthOrdinal = 0;
constexpr std::uint16_t kDamageShieldOrdinal = 1;
constexpr std::uint16_t kDamageRevisionOrdinal = 2;
/** Root ordinal of the authored scene's activation token. Signed, bias -2^31. */
constexpr std::uint16_t kSceneTokenOrdinal = 0;
/** Root ordinal of the authored scene's completion latch. A zero-width boolean. */
constexpr std::uint16_t kSceneCompletionOrdinal = 1;
/** The block's ordinal 1 is its one-bit client flag, which separates it from the task list. */
constexpr std::uint16_t kObjectiveBlockFlagOrdinal = 1;
constexpr std::uint8_t kObjectiveBlockFlagWidth = 1;
/** The client clamps a task counter here, so nothing above it is reachable. */
constexpr std::int32_t kObjectiveTaskCountLimit = 80;
/** Room for one squad takeover's log fields: two keys, two generations and two flags. */
constexpr std::size_t kSquadTakeoverFieldCapacity = 128;

namespace sense_values = middleware::bap::activity_message::sense_update;
namespace scenes = activity_sdk_mission;

/** @return The value one schema owns at one ordinal, or null when the body omits it. */
[[nodiscard]] const sense_values::DecodedValue*
sense_value(std::span<const sense_values::DecodedValue> body,
            std::uint32_t schemaRow,
            std::uint16_t ordinal) noexcept {
    for (const sense_values::DecodedValue& value : body) {
        if (value.schemaRow == schemaRow && value.fieldOrdinal == ordinal) {
            return &value;
        }
    }
    return nullptr;
}

/** @return The boolean at one ordinal. An omitted field asserts false. */
[[nodiscard]] bool sense_flag(std::span<const sense_values::DecodedValue> body,
                              std::uint32_t schemaRow,
                              std::uint16_t ordinal) noexcept {
    const sense_values::DecodedValue* const value = sense_value(body, schemaRow, ordinal);
    return value != nullptr && value->present && value->unsignedValue != 0;
}

/** @return The signed number at one ordinal. An omitted field asserts zero. */
[[nodiscard]] std::int32_t sense_number(std::span<const sense_values::DecodedValue> body,
                                        std::uint32_t schemaRow,
                                        std::uint16_t ordinal) noexcept {
    const sense_values::DecodedValue* const value = sense_value(body, schemaRow, ordinal);
    return value != nullptr && value->present ? static_cast<std::int32_t>(value->signedValue) : 0;
}

/** @return The identity every derived Sense edge carries. */
[[nodiscard]] host::Event sense_edge_event(const RuntimeInstance& instance,
                                           const host::SenseObservation& observation) noexcept {
    host::Event event{};
    event.binding = instance.view.binding;
    event.sequence = observation.sequence;
    event.tick = observation.tick;
    event.sourceGeneration = observation.sourceGeneration;
    event.missionSequence = instance.lastMissionSequence;
    event.firstRegistryKey = observation.key.registryKey;
    event.firstSlotIndex = observation.key.slotIndex;
    event.firstSlotType = observation.key.slotType;
    event.slotObjectTag = observation.key.objectTag;
    event.slotSenseSchema = observation.key.senseSchema;
    event.senseGenerationPlusOne = observation.generationPlusOne;
    event.hasSenseGeneration = observation.hasGeneration;
    return event;
}

/**
 * @return The identity a shared simulation edge carries. Its report came from the link that
 * simulates the target, but Lua and `eligible_event` read shared simulation as the route's input.
 */
[[nodiscard]] host::Event shared_edge_event(const RuntimeInstance& instance,
                                            const host::SenseObservation& observation) noexcept {
    host::Event event = sense_edge_event(instance, observation);
    event.sourceGeneration = instance.view.activityClientGeneration;
    return event;
}

/** @return A shared squad edge carrying the squad's retained spawn generation. */
[[nodiscard]] host::Event squad_edge_event(const RuntimeInstance& instance,
                                           const host::SenseObservation& observation,
                                           const SquadObservation& squad) noexcept {
    host::Event event = shared_edge_event(instance, observation);
    event.squadSpawnGeneration = squad.spawnGeneration;
    event.squadHasSpawnGeneration = squad.hasSpawnGeneration;
    return event;
}

/** @return The retained record for one squad, or null before its first observation. */
template <typename Key>
[[nodiscard]] SquadObservation* retained_squad(RuntimeInstance& instance, const Key& key) noexcept {
    for (SquadObservation& retained : instance.squadObservations) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex) {
            return &retained;
        }
    }
    return nullptr;
}

/** @return True when a squad row holds no member, alive or ever created, so it can be reused. */
[[nodiscard]] bool empty_squad(const SquadObservation& row) noexcept {
    return row.aliveCount == 0
           && std::all_of(row.slotCounts.begin(), row.slotCounts.end(), [](std::int32_t count) {
                  return count == 0;
              });
}

/** @return The retained record for one squad, allocating on a first observation. */
[[nodiscard]] SquadObservation* find_squad(RuntimeInstance& instance,
                                           const host::SenseObservationKey& key) noexcept {
    if (SquadObservation* const retained = retained_squad(instance, key)) {
        return retained;
    }
    SquadObservation* spare = nullptr;
    for (SquadObservation& retained : instance.squadObservations) {
        if (!retained.used) {
            spare = &retained;
            break;
        }
    }
    if (spare == nullptr) {
        for (SquadObservation& retained : instance.squadObservations) {
            if (empty_squad(retained)) {
                spare = &retained;
                break;
            }
        }
    }
    if (spare == nullptr) {
        log_line(core::log::Level::warn, &instance, "squad", "watch_capacity");
        return nullptr;
    }
    // An unused row can still name the link that last claimed it; a first sighting has none.
    *spare = {};
    spare->registryKey = key.registryKey;
    spare->objectTag = key.objectTag;
    spare->slotIndex = key.slotIndex;
    return spare;
}

/** @return The retained record for one objective sensor, allocating on a first observation. */
[[nodiscard]] ObjectiveObservation* find_objective(RuntimeInstance& instance,
                                                   const host::SenseObservationKey& key) noexcept {
    ObjectiveObservation* spare = nullptr;
    for (ObjectiveObservation& retained : instance.objectiveObservations) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex) {
            return &retained;
        }
        if (!retained.used && spare == nullptr) {
            spare = &retained;
        }
    }
    if (spare == nullptr) {
        log_line(core::log::Level::warn, &instance, "objective", "watch_capacity");
        return nullptr;
    }
    spare->registryKey = key.registryKey;
    spare->objectTag = key.objectTag;
    spare->slotIndex = key.slotIndex;
    return spare;
}

/** One objective sensor's task counters and the exact set the body carried. */
struct ObjectiveCounters final {
    std::array<std::array<std::uint8_t, kObjectiveTaskCapacity>, kObjectiveCapacity> value{};
    std::array<std::array<bool, kObjectiveTaskCapacity>, kObjectiveCapacity> present{};
    std::uint8_t blocks{};
};

/**
 * Copies one objective sensor's task counters out of its two nested schemas.
 * The 24 blocks share one schema row, so their order is the only separator: a block opens with its
 * own ordinal 0, then its task list. An omitted counter is absent, because zero is a real value.
 */
void objective_task_counters(std::span<const sense_values::DecodedValue> body,
                             std::uint32_t rootSchemaRow,
                             ObjectiveCounters& output) noexcept {
    output = {};
    std::uint32_t blockSchemaRow = sense_values::kAbsentRuntimeRow;
    for (const sense_values::DecodedValue& value : body) {
        if (value.schemaRow != rootSchemaRow && value.fieldOrdinal == kObjectiveBlockFlagOrdinal
            && value.width == kObjectiveBlockFlagWidth) {
            blockSchemaRow = value.schemaRow;
            break;
        }
    }
    if (blockSchemaRow == sense_values::kAbsentRuntimeRow) {
        return;
    }
    std::size_t blocks = 0;
    std::size_t tasks = 0;
    for (const sense_values::DecodedValue& value : body) {
        if (value.schemaRow == rootSchemaRow) {
            continue;
        }
        if (value.schemaRow == blockSchemaRow) {
            if (value.fieldOrdinal == 0) {
                ++blocks;
                tasks = 0;
            }
            continue;
        }
        if (blocks == 0 || blocks > output.value.size() || tasks >= kObjectiveTaskCapacity) {
            continue;
        }
        if (value.present) {
            const std::int64_t raw = value.signedValue;
            const std::int64_t clamped =
                raw <= 0 ? 0 : (std::min)(raw, std::int64_t{kObjectiveTaskCountLimit});
            output.value[blocks - 1][tasks] = static_cast<std::uint8_t>(clamped);
            output.present[blocks - 1][tasks] = true;
        }
        ++tasks;
    }
    output.blocks = static_cast<std::uint8_t>((std::min)(blocks, output.value.size()));
}

/**
 * Raises one triggerState event for a report its tracker does not drop. The event only informs;
 * entered and exited follow the level alone.
 * @param expectedSource The link whose levels the slot holds: a member's own link for a member's
 * row, else the program's route.
 * @param event The report's level, already filled in.
 */
void push_trigger_state(RuntimeInstance& instance,
                        const host::SenseObservationSnapshot& sense,
                        const host::SenseObservation& observation,
                        TriggerOccupancy& slot,
                        std::uint64_t expectedSource,
                        host::Event event) noexcept {
    trigger_observation::Snapshot level{};
    const bool valid =
        observation.valueCount <= sense.valueCount - observation.firstValue
        && trigger_observation::read(std::span(sense.values.data(), sense.valueCount)
                                         .subspan(observation.firstValue, observation.valueCount),
                                     level);
    const auto result = slot.tracker.observe(valid ? &level : nullptr,
                                             expectedSource,
                                             observation.sourceGeneration,
                                             observation.sequence,
                                             observation.generationPlusOne,
                                             observation.hasGeneration);
    if (!result.notify) {
        return;
    }
    event.kind = host::EventKind::triggerState;
    event.triggerAvailable = result.available;
    event.triggerContinuity = static_cast<std::uint8_t>(result.continuity);
    push_script_event(instance, event);
}

} // namespace

/**
 * Raises one event per watched volume whose occupancy changed, after its triggerState report.
 * The client publishes occupancy as a level, so the edge is ours to derive. A volume seen for the
 * first time only records its level, because a first observation is not an entry.
 */
void push_trigger_edges(RuntimeInstance& instance,
                        const host::SenseObservationSnapshot& sense) noexcept {
    namespace trigger = trigger_observation;
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != format::kOccupancySlotType
            || observation.valueCount < trigger::kValueCount
            || observation.firstValue + trigger::kValueCount > sense.valueCount) {
            continue;
        }
        const std::span<const sense_values::DecodedValue> body(
            &sense.values[observation.firstValue], trigger::kValueCount);
        const std::uint32_t root = observation.key.schemaRow;
        const bool occupied = sense_flag(body, root, trigger::kOccupiedOrdinal);
        TriggerOccupancy* slot = nullptr;
        TriggerOccupancy* spare = nullptr;
        auto* member = member_input(instance, sense.sourceGeneration);
        auto& levels = member ? member->triggerOccupancy : instance.triggerOccupancy;
        for (TriggerOccupancy& retained : levels) {
            if (retained.used && retained.registryKey == observation.key.registryKey
                && retained.objectTag == observation.key.objectTag
                && retained.slotIndex == observation.key.slotIndex) {
                slot = &retained;
                break;
            }
            if (!retained.used && spare == nullptr) {
                spare = &retained;
            }
        }
        const bool first = slot == nullptr;
        if (first) {
            if (spare == nullptr) {
                log_line(core::log::Level::warn, &instance, "trigger", "watch_capacity");
                continue;
            }
            slot = spare;
            slot->registryKey = observation.key.registryKey;
            slot->objectTag = observation.key.objectTag;
            slot->slotIndex = observation.key.slotIndex;
            slot->occupied = occupied;
            slot->used = true;
        }
        host::Event event = sense_edge_event(instance, observation);
        event.triggerOccupied = occupied;
        event.triggerAll = sense_flag(body, root, trigger::kAllOrdinal);
        event.triggerCount = sense_number(body, root, trigger::kCountOrdinal);
        event.triggerValue = sense_number(body, root, trigger::kThresholdOrdinal);
        // Each member's volume levels are its own, so its tracker expects its own link.
        const std::uint64_t expectedSource = member != nullptr
                                                 ? member->life.sourceGeneration
                                                 : instance.view.activityClientGeneration;
        push_trigger_state(instance, sense, observation, *slot, expectedSource, event);
        if (first || slot->occupied == occupied) {
            continue;
        }
        slot->occupied = occupied;
        event.kind = occupied ? host::EventKind::triggerEntered : host::EventKind::triggerExited;
        push_script_event(instance, event);
    }
}

namespace {

/** @return True when the observation is one complete body of the given slot type and schema. */
[[nodiscard]] bool observation_of(const host::SenseObservation& observation,
                                  const host::SenseObservationSnapshot& sense,
                                  std::uint32_t slotType,
                                  std::uint32_t senseSchema) noexcept {
    return observation.key.slotType == slotType && observation.key.senseSchema == senseSchema
           && observation.valueCount != 0 && observation.firstValue <= sense.valueCount
           && observation.valueCount <= sense.valueCount - observation.firstValue;
}

/** @return The decoded values of one observation. */
[[nodiscard]] std::span<const sense_values::DecodedValue>
observation_values(const host::SenseObservation& observation,
                   const host::SenseObservationSnapshot& sense) noexcept {
    return std::span(sense.values).subspan(observation.firstValue, observation.valueCount);
}

} // namespace

/** Raises one event per named actor whose movement or delivery level changed. */
void push_actor_path_edges(RuntimeInstance& instance,
                           const host::SenseObservationSnapshot& sense) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(observation, sense, auth::kType2SlotType, auth::kType2SenseSchema)
            || !admitted_report(instance, observation.key, sense.sourceGeneration)) {
            continue;
        }
        ActorPathObservation* const slot =
            find_slot_row(instance.actorPathObservations, observation.key);
        if (slot == nullptr) {
            continue;
        }
        claim_row(*slot, sense.sourceGeneration, true);
        if (!update_actor_path_level(
                slot->level, observation_values(observation, sense), observation.key.schemaRow)) {
            continue;
        }
        host::Event event = shared_edge_event(instance, observation);
        event.kind = host::EventKind::actorPathState;
        event.actorGeneration = slot->level.generation;
        event.actorPathRevision = slot->level.revision;
        event.actorPathState = slot->level.state;
        event.actorDeliveryRevision = slot->level.deliveryRevision;
        event.actorDeliveryState = slot->level.deliveryState;
        event.actorDeliveryKnown = (slot->level.seen & kActorSeenDelivery) == kActorSeenDelivery;
        event.actorSuppressed = slot->level.suppressed;
        std::array<char, 160> details{};
        const int written =
            std::snprintf(details.data(),
                          details.size(),
                          "registry=%08X slot=%u generation=%d revision=%d "
                          "state=%d suppressed=%u delivery_revision=%d delivery_state=%d",
                          slot->registryKey,
                          static_cast<unsigned>(slot->slotIndex),
                          slot->level.generation,
                          slot->level.revision,
                          slot->level.state,
                          slot->level.suppressed ? 1U : 0U,
                          event.actorDeliveryKnown ? slot->level.deliveryRevision : -1,
                          event.actorDeliveryKnown ? slot->level.deliveryState : -1);
        if (written > 0 && static_cast<std::size_t>(written) < details.size()) {
            log_line(core::log::Level::debug,
                     &instance,
                     "actor_path",
                     "changed",
                     {details.data(), static_cast<std::size_t>(written)});
        }
        push_script_event(instance, event);
    }
}

/** Raises one event per damage monitor whose health, shield or revision changed. */
void push_damage_edges(RuntimeInstance& instance,
                       const host::SenseObservationSnapshot& sense) noexcept {
    namespace damage = middleware::bap::activity_message::damage_monitor;
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(observation, sense, damage::kSlotType, damage::kSenseSchema)
            || !admitted_report(instance, observation.key, sense.sourceGeneration)) {
            continue;
        }
        float health = -1.0F;
        float shield = -1.0F;
        std::int32_t revision = 0;
        bool revisionKnown = false;
        for (const sense_values::DecodedValue& value : observation_values(observation, sense)) {
            if (!value.present || value.schemaRow != observation.key.schemaRow) {
                continue;
            }
            if (value.fieldOrdinal == kDamageHealthOrdinal) {
                health = value.realValue;
            } else if (value.fieldOrdinal == kDamageShieldOrdinal) {
                shield = value.realValue;
            } else if (value.fieldOrdinal == kDamageRevisionOrdinal) {
                revision = static_cast<std::int32_t>(value.signedValue);
                revisionKnown = true;
            }
        }
        if (!revisionKnown || revision <= 0 || !std::isfinite(health) || !std::isfinite(shield)) {
            continue;
        }
        DamageObservation* const row = find_slot_row(instance.damageObservations, observation.key);
        if (row == nullptr) {
            continue;
        }
        claim_row(*row, sense.sourceGeneration, true);
        if (revision < row->revision
            || (row->revision == revision && row->health == health && row->shield == shield)) {
            continue;
        }
        row->revision = revision;
        row->health = health;
        row->shield = shield;
        host::Event event = shared_edge_event(instance, observation);
        event.kind = host::EventKind::damageState;
        event.damageHealth = health;
        event.damageShield = shield;
        event.damageRevision = revision;
        std::array<char, 128> details{};
        const int written =
            std::snprintf(details.data(),
                          details.size(),
                          "registry=%08X slot=%u revision=%d health=%.4f shield=%.4f",
                          observation.key.registryKey,
                          static_cast<unsigned>(observation.key.slotIndex),
                          revision,
                          static_cast<double>(health),
                          static_cast<double>(shield));
        if (written > 0 && static_cast<std::size_t>(written) < details.size()) {
            log_line(core::log::Level::debug,
                     &instance,
                     "damage",
                     "changed",
                     {details.data(), static_cast<std::size_t>(written)});
        }
        push_script_event(instance, event);
    }
}

namespace {

/**
 * Copies one object level into the fields its object events carry.
 * @param before The row's level before this report, which says whether it is a first report.
 */
void copy_object_level(const ObjectInteractionLevel& before,
                       const ObjectInteractionLevel& level,
                       host::Event& event) noexcept {
    event.initialObservation = !before.stateKnown || before.generation != level.generation;
    event.objectEntryIndex = level.entryIndex;
    event.objectSpawnMask = level.spawnMask;
    event.objectGeneration = level.generation;
    event.objectPresent = level.present;
    // The alive lane carries Sense ordinal 1, which the script reads as interaction_open.
    event.objectAlive = level.interactionOpen;
    event.objectOwnerKnown = level.ownerKnown;
    event.objectHasOwner = level.hasOwner;
    event.objectOwnerKey = level.ownerKey;
}

/**
 * Raises object state from the link that simulates the object. Every client that replicates the
 * object reports the same world state, so only that link's history raises it.
 */
void push_object_state_edge(RuntimeInstance& instance,
                            const host::SenseObservationSnapshot& sense,
                            const host::SenseObservation& observation) noexcept {
    ObjectInteractionObservation* const slot =
        find_slot_row(instance.objectStateObservations, observation.key);
    if (slot == nullptr) {
        return;
    }
    claim_row(*slot, sense.sourceGeneration, true);
    const ObjectInteractionLevel before = slot->level;
    static_cast<void>(update_object_interaction(
        slot->level, observation_values(observation, sense), observation.key.schemaRow));
    const ObjectInteractionLevel& level = slot->level;
    const bool stateChanged =
        level.generationKnown && level.generation > 0 && level.stateKnown
        && (!before.stateKnown || before.generation != level.generation
            || before.present != level.present || before.interactionOpen != level.interactionOpen
            || before.ownerKnown != level.ownerKnown || before.hasOwner != level.hasOwner
            || before.ownerKey != level.ownerKey);
    if (!stateChanged) {
        return;
    }
    host::Event event = shared_edge_event(instance, observation);
    copy_object_level(before, level, event);
    event.kind = host::EventKind::objectState;
    std::array<char, 128> details{};
    const int written = std::snprintf(details.data(),
                                      details.size(),
                                      "registry=%08X slot=%u generation=%d present=%u "
                                      "interaction_open=%u owner_known=%u has_owner=%u",
                                      observation.key.registryKey,
                                      static_cast<unsigned>(observation.key.slotIndex),
                                      level.generation,
                                      level.present ? 1U : 0U,
                                      level.interactionOpen ? 1U : 0U,
                                      level.ownerKnown ? 1U : 0U,
                                      level.hasOwner ? 1U : 0U);
    if (written > 0 && static_cast<std::size_t>(written) < details.size()) {
        log_line(core::log::Level::debug,
                 &instance,
                 "object",
                 "changed",
                 {details.data(), static_cast<std::size_t>(written)});
    }
    push_script_event(instance, event);
}

} // namespace

/**
 * Raises object state from the object's simulating link, and each member's accepted interactions
 * from that member's own levels, since an interaction is one actor's.
 */
void push_object_interaction_edges(RuntimeInstance& instance,
                                   const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(
                observation, sense, format::kObjectSlotType, format::kObjectSenseSchema)) {
            continue;
        }
        if (admitted_report(instance, observation.key, sense.sourceGeneration)) {
            push_object_state_edge(instance, sense, observation);
        }
        auto* member = member_input(instance, sense.sourceGeneration);
        ObjectInteractionObservation* const slot = find_slot_row(
            member ? member->objectInteractionObservations : instance.objectInteractionObservations,
            observation.key);
        if (slot == nullptr) {
            continue;
        }
        const ObjectInteractionLevel before = slot->level;
        if (!update_object_interaction(
                slot->level, observation_values(observation, sense), observation.key.schemaRow)) {
            continue;
        }
        host::Event event = sense_edge_event(instance, observation);
        copy_object_level(before, slot->level, event);
        event.kind = host::EventKind::objectInteracted;
        push_script_event(instance, event);
    }
}

/** Raises one event per Ghost link whose level changed. */
void push_ghost_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept {
    namespace ghost = middleware::bap::activity_message::ghost_link;
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(observation, sense, ghost::kSlotType, ghost::kSenseSchema)) {
            continue;
        }
        auto* member = member_input(instance, sense.sourceGeneration);
        GhostObservation* const slot = find_slot_row(
            member ? member->ghostObservations : instance.ghostObservations, observation.key);
        if (slot == nullptr
            || !update_ghost_level(
                slot->level, observation_values(observation, sense), observation.key.schemaRow)) {
            continue;
        }
        host::Event event = sense_edge_event(instance, observation);
        event.kind = host::EventKind::ghostLinkState;
        event.ghostGeneration = slot->level.generation;
        event.ghostProgress = slot->level.progress;
        event.ghostActive = slot->level.active;
        push_script_event(instance, event);
    }
    publish_ghost_levels(instance);
}

/**
 * Publishes each Ghost link's levels from one member's history, never mixing two: the member whose
 * client simulates the link, else the route's member when that member is not present.
 */
void publish_ghost_levels(RuntimeInstance& instance) noexcept {
    namespace ghost = middleware::bap::activity_message::ghost_link;
    std::array<GhostLinkRow, kGhostObservationCapacity> rows{};
    std::size_t count = 0;
    const std::uint64_t route = instance.view.activityClientGeneration;
    const auto present = [&](std::uint64_t generation) noexcept {
        return std::any_of(instance.memberInputs.begin(),
                           instance.memberInputs.end(),
                           [generation](const MemberInputState& member) noexcept {
                               return member.present && member.life.sourceGeneration == generation;
                           });
    };
    const auto publish = [&](const auto& observations, std::uint64_t generation) noexcept {
        for (const GhostObservation& observation : observations) {
            const OutputClientRef ref{.registryKey = observation.registryKey,
                                      .slotIndex = observation.slotIndex,
                                      .slotType = ghost::kSlotType};
            if (!observation.used || count == rows.size()) {
                continue;
            }
            const std::uint64_t simulating = simulating_link(instance, ref);
            if ((present(simulating) ? simulating : route) == generation) {
                rows[count++] = {observation.level,
                                 observation.registryKey,
                                 observation.objectTag,
                                 observation.slotIndex};
            }
        }
    };
    for (const MemberInputState& member : instance.memberInputs) {
        if (member.present && member.life.sourceGeneration != 0) {
            publish(member.ghostObservations, member.life.sourceGeneration);
        }
    }
    if (member_input(instance, route) == nullptr) {
        publish(instance.ghostObservations, route);
    }
    lua_vm::publish_ghost_levels(instance.vm, std::span(rows).first(count));
}

namespace {

/** One squad report read over a row's levels; a report restates only the fields that moved. */
struct SquadLevels final {
    std::array<std::int32_t, host::kSquadSlotCapacity> counts{};
    std::int32_t alive{};
    std::uint8_t countLength{};
    bool removal{};
    bool costsChanged{};
    /** The report carried an alive count. */
    bool hasAlive{};
    /** False when the report restated no level the row keeps. */
    bool reported{};
};

/** Merges one report's objective costs into the row and reads its other levels over the row's. */
[[nodiscard]] SquadLevels read_squad_levels(SquadObservation& row,
                                            std::span<const sense_values::DecodedValue> body,
                                            std::uint32_t root) noexcept {
    SquadLevels levels{};
    levels.costsChanged = update_squad_objective_costs(row.objectiveCosts, body, root);
    // Cost-only deltas retain combat counts; they must never manufacture a death.
    levels.alive = row.aliveCount;
    levels.hasAlive = read_squad_alive(body, root, levels.alive);
    levels.removal = sense_flag(body, root, kSquadRemovalOrdinal);
    levels.counts = row.slotCounts;
    levels.countLength = row.slotCountLength;
    std::array<std::int32_t, host::kSquadSlotCapacity> incomingCounts{};
    const auto incomingLength = read_squad_created_counts(body, incomingCounts);
    if (incomingLength != 0) {
        levels.counts = incomingCounts;
        levels.countLength = incomingLength;
    }
    levels.reported = levels.hasAlive || levels.costsChanged || incomingLength != 0;
    return levels;
}

/**
 * Retains what a reported squad says about its own placement: whether an alive count was ever
 * reported, and the spawn generation it echoes. An absent spawn generation keeps the last one
 * reported, like the other retained levels.
 */
void retain_squad_identity(SquadObservation& row,
                           std::span<const sense_values::DecodedValue> body,
                           std::uint32_t root,
                           bool hasAlive) noexcept {
    row.populationKnown = row.populationKnown || hasAlive;
    const sense_values::DecodedValue* const spawn =
        sense_value(body, root, kSquadSpawnGenerationOrdinal);
    if (spawn != nullptr && spawn->present && spawn->signedValue >= 0
        && spawn->signedValue <= kMaximumCounter) {
        row.spawnGeneration = static_cast<std::int32_t>(spawn->signedValue);
        row.hasSpawnGeneration = true;
    }
}

void store_squad_levels(SquadObservation& row, const SquadLevels& levels) noexcept {
    row.aliveCount = levels.alive;
    row.removalFlag = levels.removal;
    row.slotCounts = levels.counts;
    row.slotCountLength = levels.countLength;
}

/** @return The levels a row holds now, as its edges read them. */
[[nodiscard]] SquadPublishedLevels current_squad_levels(const SquadObservation& row) noexcept {
    return {.objectiveCosts = row.objectiveCosts,
            .slotCounts = row.slotCounts,
            .aliveCount = row.aliveCount,
            .slotCountLength = row.slotCountLength,
            .removalFlag = row.removalFlag};
}

/** @return True when one generation is a live link of this pass. */
[[nodiscard]] bool live_link(const RuntimeInstance& instance, std::uint64_t generation) noexcept {
    const auto live = live_links(instance);
    return std::find(live.begin(), live.end(), generation) != live.end();
}

/**
 * @return A shadow whose squad no pin of this pass names and no main row holds. Only a later
 * delivery can make it its link's baseline, so it is the first used shadow to give way.
 */
[[nodiscard]] SquadObservation* unclaimed_shadow(RuntimeInstance& instance) noexcept {
    const auto pins = pass_pins(instance);
    for (SquadObservation& shadow : instance.squadShadows) {
        if (shadow.used && retained_squad(instance, shadow) == nullptr
            && std::none_of(pins.begin(), pins.end(), [&shadow](const host::ScriptablePin& pin) {
                   return pin.registryKey == shadow.registryKey;
               })) {
            return &shadow;
        }
    }
    return nullptr;
}

/**
 * @return One link's shadow of a squad. With `allocate`, a missing one takes a free row, else a row
 * whose link left, else a row whose squad holds no member and no objective cost, since a sparse
 * delta never restates those, else an unclaimed squad's row; null when there is none.
 */
template <typename Key>
[[nodiscard]] SquadObservation* find_squad_shadow(RuntimeInstance& instance,
                                                  const Key& key,
                                                  std::uint64_t sourceGeneration,
                                                  bool allocate) noexcept {
    SquadObservation* spare = nullptr;
    SquadObservation* departed = nullptr;
    SquadObservation* empty = nullptr;
    for (SquadObservation& shadow : instance.squadShadows) {
        if (!shadow.used) {
            if (spare == nullptr) {
                spare = &shadow;
            }
            continue;
        }
        if (shadow.sourceGeneration == sourceGeneration && shadow.registryKey == key.registryKey
            && shadow.objectTag == key.objectTag && shadow.slotIndex == key.slotIndex) {
            return &shadow;
        }
        if (departed == nullptr && !live_link(instance, shadow.sourceGeneration)) {
            departed = &shadow;
        }
        if (empty == nullptr && empty_squad(shadow)
            && shadow.objectiveCosts == SquadObjectiveCosts{}) {
            empty = &shadow;
        }
    }
    if (!allocate) {
        return nullptr;
    }
    SquadObservation* row = spare;
    if (row == nullptr) {
        row = departed != nullptr ? departed : empty;
    }
    if (row == nullptr) {
        row = unclaimed_shadow(instance);
    }
    if (row == nullptr) {
        // Every later report of a squad without a shadow lands here too.
        if (!instance.squadShadowAtCapacity) {
            log_line(core::log::Level::warn, &instance, "squad", "shadow_capacity");
        }
        instance.squadShadowAtCapacity = true;
        return nullptr;
    }
    instance.squadShadowAtCapacity = false;
    *row = {};
    row->sourceGeneration = sourceGeneration;
    row->registryKey = key.registryKey;
    row->objectTag = key.objectTag;
    row->slotIndex = key.slotIndex;
    return row;
}

/**
 * Keeps a link's squad levels while another link simulates the squad, raising nothing. A link whose
 * levels are still the squad's row keeps them there, so each link's levels live in one row and a
 * takeover moves them whole. A link that left never simulates the squad again.
 */
void shadow_squad_report(RuntimeInstance& instance,
                         const host::SenseObservation& observation,
                         std::span<const sense_values::DecodedValue> body,
                         std::uint64_t sourceGeneration) noexcept {
    if (!live_link(instance, sourceGeneration)) {
        return;
    }
    SquadObservation* shadow = retained_squad(instance, observation.key);
    if (shadow == nullptr || shadow->sourceGeneration != sourceGeneration) {
        shadow = find_squad_shadow(instance, observation.key, sourceGeneration, true);
    }
    if (shadow == nullptr) {
        return;
    }
    const SquadLevels levels = read_squad_levels(*shadow, body, observation.key.schemaRow);
    if (!levels.reported) {
        return;
    }
    shadow->used = true;
    retain_squad_identity(*shadow, body, observation.key.schemaRow, levels.hasAlive);
    store_squad_levels(*shadow, levels);
}

/** Logs a squad row changing links, so a boot log shows each re-pin window. */
void log_squad_takeover(const RuntimeInstance& instance,
                        const SquadObservation& row,
                        std::uint64_t sourceGeneration,
                        bool shadowed,
                        bool kept) noexcept {
    std::array<char, kSquadTakeoverFieldCapacity> fields{};
    const int written = std::snprintf(fields.data(),
                                      fields.size(),
                                      "registry=%08X slot=%u from=%llu to=%llu shadow=%d kept=%d",
                                      row.registryKey,
                                      static_cast<unsigned>(row.slotIndex),
                                      static_cast<unsigned long long>(row.sourceGeneration),
                                      static_cast<unsigned long long>(sourceGeneration),
                                      shadowed ? 1 : 0,
                                      kept ? 1 : 0);
    if (written > 0 && static_cast<std::size_t>(written) < fields.size()) {
        log_line(core::log::Level::info,
                 &instance,
                 "squad",
                 "takeover",
                 {fields.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Hands a squad's row to the link whose report was admitted. That link's own levels are the ones it
 * reported while another link simulated the squad, so its shadow becomes the row, and the displaced
 * levels become their link's one shadow, in case that link simulates the squad again.
 * @return True when the row took over levels the link had reported.
 */
[[nodiscard]] bool take_over_squad(RuntimeInstance& instance,
                                   SquadObservation& row,
                                   std::uint64_t sourceGeneration) noexcept {
    SquadObservation* const shadow = find_squad_shadow(instance, row, sourceGeneration, false);
    SquadObservation taken{};
    if (shadow != nullptr) {
        taken = *shadow;
        *shadow = {};
    }
    bool kept = false;
    if (row.used && live_link(instance, row.sourceGeneration)) {
        if (SquadObservation* const displaced =
                find_squad_shadow(instance, row, row.sourceGeneration, true)) {
            *displaced = row;
            kept = true;
        }
    }
    // A first sighting claims an unused row from no link; only a used row changes hands.
    if (row.used) {
        log_squad_takeover(instance, row, sourceGeneration, shadow != nullptr, kept);
    }
    if (shadow == nullptr) {
        claim_row(row, sourceGeneration, false);
        return false;
    }
    row = taken;
    return true;
}

} // namespace

/**
 * Raises the squad events derived from one msg 6 body.
 * The client publishes levels, so a first sighting of a source only records them. After that a
 * slot count that rose is the only spawn signal, and the alive count falling is the only death
 * signal. Only the link that simulates the squad reports its levels; a replica's counts would
 * interleave with them, so they are kept apart as that link's shadow.
 */
void push_squad_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.sourceGeneration != sense.sourceGeneration
            || observation.key.slotType != format::kSquadSlotType
            || observation.key.senseSchema != format::kSquadSenseSchema
            || observation.firstValue + observation.valueCount > sense.valueCount) {
            continue;
        }
        const std::span<const sense_values::DecodedValue> body(
            &sense.values[observation.firstValue], observation.valueCount);
        const std::uint32_t root = observation.key.schemaRow;
        if (!admitted_report(instance, observation.key, sense.sourceGeneration)) {
            if (sense.sourceGeneration != 0) {
                shadow_squad_report(instance, observation, body, sense.sourceGeneration);
            }
            continue;
        }
        // The placement's count evidence and its retirement follow the squad to the link that
        // simulates it now.
        const SquadObservation* const retained = retained_squad(instance, observation.key);
        const bool moved =
            retained != nullptr && retained->sourceGeneration != sense.sourceGeneration;
        server::gameplay::squad_entity_retirement::follow_squad(
            instance.view.binding,
            sense.sourceGeneration,
            {observation.key.registryKey, observation.key.slotIndex, observation.key.slotType},
            live_links(instance));
        if (!observe_population(instance, observation, body, moved)) {
            return;
        }
        // Accepting the population can start a new attempt, which clears every row.
        SquadObservation* const squad = find_squad(instance, observation.key);
        if (squad == nullptr) {
            continue;
        }
        const bool rebaselined = squad->sourceGeneration != sense.sourceGeneration
                                 && take_over_squad(instance, *squad, sense.sourceGeneration);
        const SquadLevels levels = read_squad_levels(*squad, body, root);
        const SquadPublishedLevels& published = squad->published;
        // A fold leaves the row ahead of the script, so a delta that restates nothing raises it.
        if (!rebaselined && !levels.reported && current_squad_levels(*squad) == published) {
            continue;
        }
        // Levels taken over at a re-pin are a baseline, so they raise no spawn or death.
        const bool first = rebaselined || !squad->used;
        squad->used = true;
        retain_squad_identity(*squad, body, root, levels.hasAlive);
        const std::int32_t previousAlive = first ? 0 : published.aliveCount;
        const bool changed = first || squad->objectiveCosts != published.objectiveCosts
                             || published.aliveCount != levels.alive
                             || published.removalFlag != levels.removal
                             || published.slotCountLength != levels.countLength
                             || published.slotCounts != levels.counts;
        if (!changed) {
            store_squad_levels(*squad, levels);
            continue;
        }
        host::Event state = squad_edge_event(instance, observation, *squad);
        state.squadObjectiveCosts = squad->objectiveCosts.values;
        state.squadObjectiveCostMask = squad->objectiveCosts.known;
        state.squadObjectiveRevision = squad->objectiveCosts.revision;
        state.squadAliveCount = levels.alive;
        state.squadPreviousAliveCount = previousAlive;
        state.squadPopulationAvailable = squad->populationKnown;
        state.squadRemovalFlag = levels.removal;
        state.squadSlotCounts = levels.counts;
        state.squadSlotCountLength = levels.countLength;
        state.initialObservation = first;
        state.kind = host::EventKind::squadState;
        if (instance.programStatus == ProgramStatus::loaded) {
            if (!qualify_objective_costs(instance, state)) {
                return;
            }
            push_script_event(instance, state);
        }

        // A replacement source starts from its own levels, so only a rise after the first sighting
        // is a spawn.
        for (std::uint8_t slot = 0; !first && slot < levels.countLength; ++slot) {
            const std::int32_t previous =
                slot < published.slotCountLength ? published.slotCounts[slot] : 0;
            if (levels.counts[slot] <= previous) {
                continue;
            }
            host::Event spawned = squad_edge_event(instance, observation, *squad);
            spawned.squadSlotOrdinal = slot;
            spawned.squadSlotValue = levels.counts[slot];
            spawned.squadPreviousSlotValue = previous;
            spawned.kind = host::EventKind::entitySpawned;
            if (instance.programStatus == ProgramStatus::loaded) {
                push_script_event(instance, spawned);
            }
        }
        if (!first && levels.alive < published.aliveCount) {
            host::Event died = squad_edge_event(instance, observation, *squad);
            died.squadAliveCount = levels.alive;
            died.squadPreviousAliveCount = published.aliveCount;
            died.squadRemovalFlag = levels.removal;
            died.kind = host::EventKind::entityDied;
            if (instance.programStatus == ProgramStatus::loaded) {
                push_script_event(instance, died);
            }
        }
        store_squad_levels(*squad, levels);
        squad->published = current_squad_levels(*squad);
    }
}

/** Every report whose admission or publication reads which link simulates its target. */
bool reads_pins(const host::SenseObservationSnapshot& sense) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    namespace damage = middleware::bap::activity_message::damage_monitor;
    namespace ghost = middleware::bap::activity_message::ghost_link;
    static constexpr std::array kSharedSlotTypes{
        static_cast<std::uint32_t>(format::kSquadSlotType),
        static_cast<std::uint32_t>(auth::kType2SlotType),
        static_cast<std::uint32_t>(kCombatantDamageSlotType),
        static_cast<std::uint32_t>(damage::kSlotType),
        static_cast<std::uint32_t>(kDeviceSenseSlotType),
        static_cast<std::uint32_t>(format::kAuthoredSceneSlotType),
        static_cast<std::uint32_t>(format::kObjectiveSlotType),
        static_cast<std::uint32_t>(format::kObjectSlotType),
        static_cast<std::uint32_t>(ghost::kSlotType),
    };
    const std::size_t count = (std::min)(sense.observationCount, sense.observations.size());
    return std::any_of(sense.observations.begin(),
                       sense.observations.begin() + static_cast<std::ptrdiff_t>(count),
                       [](const host::SenseObservation& observation) noexcept {
                           return std::find(kSharedSlotTypes.begin(),
                                            kSharedSlotTypes.end(),
                                            observation.key.slotType)
                                  != kSharedSlotTypes.end();
                       });
}

/** @return The scene's gate rows; empty when its slot does not resolve. */
[[nodiscard]] std::span<const format::AuthoredSceneEventKey>
scene_gate_rows(const RuntimeInstance& instance, const host::SenseObservationKey& key) noexcept {
    if (instance.view.catalog == nullptr) {
        return {};
    }
    const slot_index::SenseKey indexKey{.registryKey = key.registryKey,
                                        .objectTag = key.objectTag,
                                        .senseSchema = key.senseSchema,
                                        .slotIndex = key.slotIndex,
                                        .slotType = key.slotType};
    slot_index::Found found{};
    if (!slot_index::by_sense(instance.view, indexKey, found)) {
        return {};
    }
    return sdk::scene_gates(instance.view.catalog->authored_scene_event_keys(), found.nativeRow);
}

/**
 * Records the output keys a scene body lists, raising one event per key new to this run.
 * Any root value equal to an output key counts; the keys are name hashes, so no other field
 * carries one.
 */
void record_scene_outputs(RuntimeInstance& instance,
                          const host::SenseObservation& observation,
                          std::span<const sense_values::DecodedValue> body,
                          std::uint32_t root,
                          SceneObservation& scene,
                          bool raise) noexcept {
    const auto gates = scene_gate_rows(instance, observation.key);
    for (const sense_values::DecodedValue& value : body) {
        const auto key = static_cast<std::uint32_t>(value.unsignedValue);
        if (value.schemaRow != root || !value.present
            || std::find(scene.outputs.begin(), scene.outputs.begin() + scene.outputCount, key)
                   != scene.outputs.begin() + scene.outputCount
            || std::none_of(gates.begin(), gates.end(), [key](const auto& gate) {
                   return gate.key == key && sdk::scene_trigger_is_output(gate);
               })) {
            continue;
        }
        if (scene.outputCount == scene.outputs.size()) {
            log_line(core::log::Level::warn, &instance, "scene", "output_capacity");
            return;
        }
        scene.outputs[scene.outputCount++] = key;
        if (!raise) {
            continue;
        }
        host::Event event = shared_edge_event(instance, observation);
        event.sceneActivationToken = scene.token;
        event.sceneOutputKey = key;
        event.kind = host::EventKind::sceneOutput;
        push_script_event(instance, event);
    }
}

/**
 * Raises one event per watched authored scene that latched complete, and one per output gate a
 * scene newly lists. The completion field is a latch, so only the false to true edge is a finish.
 * A scene seen for the first time records its level and outputs and raises nothing.
 */
void push_scene_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != format::kAuthoredSceneSlotType
            || observation.key.senseSchema != format::kAuthoredSceneSenseSchema
            || observation.firstValue + observation.valueCount > sense.valueCount
            || !admitted_report(instance, observation.key, sense.sourceGeneration)) {
            continue;
        }
        const std::span<const sense_values::DecodedValue> body(
            &sense.values[observation.firstValue], observation.valueCount);
        const std::uint32_t root = observation.key.schemaRow;
        // An unchanged scene sends an empty delta, which must not read as a completion of false.
        const bool hasCompletion = sense_value(body, root, kSceneCompletionOrdinal) != nullptr;
        const bool completed = sense_flag(body, root, kSceneCompletionOrdinal);
        const sense_values::DecodedValue* const token = sense_value(body, root, kSceneTokenOrdinal);
        SceneObservation* slot = nullptr;
        SceneObservation* spare = nullptr;
        for (SceneObservation& retained : instance.sceneObservations) {
            if (retained.used && retained.registryKey == observation.key.registryKey
                && retained.objectTag == observation.key.objectTag
                && retained.slotIndex == observation.key.slotIndex) {
                slot = &retained;
                break;
            }
            if (!retained.used && spare == nullptr) {
                spare = &retained;
            }
        }
        // Another link's latch and outputs are not this link's; its run starts as a first sighting.
        if (slot != nullptr && slot->sourceGeneration != sense.sourceGeneration) {
            spare = slot;
            slot = nullptr;
        }
        if (slot == nullptr) {
            if (spare == nullptr) {
                log_line(core::log::Level::warn, &instance, "scene", "watch_capacity");
                continue;
            }
            *spare = {};
            spare->sourceGeneration = sense.sourceGeneration;
            spare->registryKey = observation.key.registryKey;
            spare->objectTag = observation.key.objectTag;
            spare->slotIndex = observation.key.slotIndex;
            spare->completed = completed;
            spare->token = sense_number(body, root, kSceneTokenOrdinal);
            spare->used = true;
            record_scene_outputs(instance, observation, body, root, *spare, false);
            continue;
        }
        // A new activation token is a new run of the scene, whose outputs start empty.
        if (token != nullptr && token->present
            && static_cast<std::int32_t>(token->signedValue) != slot->token) {
            slot->token = static_cast<std::int32_t>(token->signedValue);
            slot->outputCount = 0;
        }
        record_scene_outputs(instance, observation, body, root, *slot, true);
        if (!hasCompletion) {
            continue;
        }
        const bool rising = completed && !slot->completed;
        slot->completed = completed;
        if (!rising) {
            continue;
        }
        host::Event event = shared_edge_event(instance, observation);
        event.sceneActivationToken = sense_number(body, root, kSceneTokenOrdinal);
        event.kind = host::EventKind::sceneFinished;
        push_script_event(instance, event);
    }
}

/**
 * Raises one event per watched objective task counter that rose.
 * The client owns the counter and raises it on an actor teardown, so a rise is the only progress
 * signal. A sensor seen for the first time records its counters and raises nothing.
 */
void push_objective_edges(RuntimeInstance& instance,
                          const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != format::kObjectiveSlotType
            || observation.key.senseSchema != format::kObjectiveSenseSchema
            || observation.firstValue + observation.valueCount > sense.valueCount
            || !admitted_report(instance, observation.key, sense.sourceGeneration)) {
            continue;
        }
        const std::span<const sense_values::DecodedValue> body(
            &sense.values[observation.firstValue], observation.valueCount);
        ObjectiveCounters counters{};
        objective_task_counters(body, observation.key.schemaRow, counters);
        // An unchanged sensor sends an empty delta, which must not read as every counter at zero.
        if (counters.blocks == 0) {
            continue;
        }
        ObjectiveObservation* const watched = find_objective(instance, observation.key);
        if (watched == nullptr) {
            continue;
        }
        claim_row(*watched, sense.sourceGeneration, false);
        const bool first = !watched->used;
        watched->used = true;
        for (std::uint8_t block = 0; block < counters.blocks; ++block) {
            for (std::size_t task = 0; task < kObjectiveTaskCapacity; ++task) {
                if (!counters.present[block][task]) {
                    continue;
                }
                const std::uint8_t value = counters.value[block][task];
                const std::uint8_t previous = watched->counters[block][task];
                watched->counters[block][task] = value;
                if (first || value <= previous) {
                    continue;
                }
                host::Event event = shared_edge_event(instance, observation);
                event.objectiveOrdinal = block;
                event.objectiveTaskOrdinal = static_cast<std::uint8_t>(task);
                event.objectiveTaskCount = value;
                event.objectivePreviousTaskCount = previous;
                event.kind = host::EventKind::objectiveProgress;
                push_script_event(instance, event);
            }
        }
    }
}

} // namespace sunrise::server::activity::mission
