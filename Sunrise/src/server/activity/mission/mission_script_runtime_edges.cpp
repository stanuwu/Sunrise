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
#include "../activity_sdk_mission_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_runtime_objective.h"

// Sense events retain levels within the owning mission attempt.

namespace sunrise::server::activity::mission {
namespace {

/** Root ordinal of the squad removal flag. The rest of the root is inert on this build. */
constexpr std::uint16_t kSquadRemovalOrdinal = 8;
/** Root ordinal 0 echoes the squad Auth .6 spawn generation on build 86657. */
constexpr std::uint16_t kSquadSpawnGenerationOrdinal = 0;
/** Root ordinals of the type-20 damage Sense body: health, shield, then the echoed revision. */
constexpr std::uint16_t kDamageHealthOrdinal = 0;
constexpr std::uint16_t kDamageShieldOrdinal = 1;
constexpr std::uint16_t kDamageRevisionOrdinal = 2;
/** Root ordinal of the authored scene's activation token. Signed, bias -2^31. */
constexpr std::uint16_t kSceneTokenOrdinal = 0;
/** Root ordinal of the authored scene's completion latch. A zero-width boolean. */
constexpr std::uint16_t kSceneCompletionOrdinal = 1;
/** Objective sensors publish on this Sense slot type. */
constexpr std::uint8_t kObjectiveSlotType = 3;
/** Objective sensor Sense root schema. */
constexpr std::uint32_t kObjectiveSenseSchema = 0x80807F04U;
/** The block's ordinal 1 is its one-bit client flag, which separates it from the task list. */
constexpr std::uint16_t kObjectiveBlockFlagOrdinal = 1;
constexpr std::uint8_t kObjectiveBlockFlagWidth = 1;
/** The client clamps a task counter here, so nothing above it is reachable. */
constexpr std::int32_t kObjectiveTaskCountLimit = 80;

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
    event.sourceGeneration = instance.view.activityClientGeneration;
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

/** @return The retained record for one squad, allocating on a first observation. */
[[nodiscard]] SquadObservation* find_squad(RuntimeInstance& instance,
                                           const host::SenseObservationKey& key) noexcept {
    SquadObservation* spare = nullptr;
    for (SquadObservation& retained : instance.squadObservations) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex
            && retained.slotType == key.slotType && retained.schemaRow == key.schemaRow) {
            return &retained;
        }
        if (!retained.used && spare == nullptr) {
            spare = &retained;
        }
    }
    if (spare == nullptr) {
        for (SquadObservation& retained : instance.squadObservations) {
            bool empty = retained.aliveCount == 0;
            for (const auto count : retained.slotCounts) {
                empty = empty && count == 0;
            }
            if (empty) {
                retained = {};
                spare = &retained;
                break;
            }
        }
    }
    if (spare == nullptr) {
        log_line(core::log::Level::warn, &instance, "squad", "watch_capacity");
        return nullptr;
    }
    spare->registryKey = key.registryKey;
    spare->objectTag = key.objectTag;
    spare->slotIndex = key.slotIndex;
    spare->slotType = key.slotType;
    spare->schemaRow = key.schemaRow;
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

} // namespace

/**
 * Publishes qualified trigger levels and derives ordinary enter/exit edges from consecutive levels.
 * A first observation publishes a baseline state, because a first observation is not an entry.
 */
void push_trigger_edges(RuntimeInstance& instance,
                        const host::SenseObservationSnapshot& sense,
                        std::uint64_t missionSequence) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != trigger_observation::kSlotType
            || observation.key.senseSchema != trigger_observation::kSchema
            || observation.key.schemaRow != trigger_observation::kSchema
            || observation.sourceGeneration == 0
            || observation.sourceGeneration != instance.view.activityClientGeneration
            || observation.sequence == 0) {
            continue;
        }
        TriggerOccupancy* slot = nullptr;
        TriggerOccupancy* spare = nullptr;
        for (TriggerOccupancy& retained : instance.triggerOccupancy) {
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
        if (slot == nullptr) {
            if (spare == nullptr) {
                log_line(core::log::Level::warn, &instance, "trigger", "watch_capacity");
                continue;
            }
            slot = spare;
            slot->registryKey = observation.key.registryKey;
            slot->objectTag = observation.key.objectTag;
            slot->slotIndex = observation.key.slotIndex;
            slot->used = true;
        }
        trigger_observation::Snapshot current{};
        // An exact source with a malformed body must invalidate the prior baseline as well.
        const bool bounded = observation.firstValue <= sense.valueCount
                             && observation.valueCount <= sense.valueCount - observation.firstValue;
        const bool valid = bounded
                           && trigger_observation::read(
                               std::span(sense.values.data(), sense.valueCount)
                                   .subspan(observation.firstValue, observation.valueCount),
                               current);
        const auto result = slot->tracker.observe(valid ? &current : nullptr,
                                                  instance.view.activityClientGeneration,
                                                  observation.sourceGeneration,
                                                  observation.sequence,
                                                  observation.generationPlusOne,
                                                  observation.hasGeneration);
        if (!result.notify) {
            continue;
        }
        host::Event event = sense_edge_event(instance, observation);
        event.missionSequence = missionSequence;
        event.kind = host::EventKind::triggerState;
        event.triggerAvailable = result.available;
        event.triggerOccupied = current.occupied;
        event.triggerContinuity = static_cast<std::uint8_t>(result.continuity);
        event.triggerCount = current.count;
        event.triggerValue = current.threshold;
        event.triggerAll = current.all;
        push_script_event(instance, event);
        if (result.edge != trigger_observation::Edge::none) {
            event.kind = result.edge == trigger_observation::Edge::entered
                             ? host::EventKind::triggerEntered
                             : host::EventKind::triggerExited;
            push_script_event(instance, event);
        }
    }
}

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

/**
 * Finds the retained row for one slot, allocating a free row on a first observation.
 * @return Null when every row is in use.
 */
template <typename Row, std::size_t N>
[[nodiscard]] Row* find_slot_row(std::array<Row, N>& rows,
                                 const host::SenseObservationKey& key) noexcept {
    Row* spare = nullptr;
    for (Row& retained : rows) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex) {
            return &retained;
        }
        if (!retained.used && spare == nullptr) {
            spare = &retained;
        }
    }
    if (spare != nullptr) {
        spare->used = true;
        spare->registryKey = key.registryKey;
        spare->objectTag = key.objectTag;
        spare->slotIndex = key.slotIndex;
    }
    return spare;
}

/** Raises one event per named actor whose movement or delivery level changed. */
void push_actor_path_edges(RuntimeInstance& instance,
                           const host::SenseObservationSnapshot& sense) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(observation, sense, auth::kType2SlotType, auth::kType2SenseSchema)) {
            continue;
        }
        ActorPathObservation* const slot =
            find_slot_row(instance.actorPathObservations, observation.key);
        if (slot == nullptr
            || !update_actor_path_level(
                slot->level, observation_values(observation, sense), observation.key.schemaRow)) {
            continue;
        }
        host::Event event = sense_edge_event(instance, observation);
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
        if (!observation_of(observation, sense, damage::kSlotType, damage::kSenseSchema)) {
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
        if (row == nullptr || revision < row->revision
            || (row->revision == revision && row->health == health && row->shield == shield)) {
            continue;
        }
        row->revision = revision;
        row->health = health;
        row->shield = shield;
        host::Event event = sense_edge_event(instance, observation);
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

/**
 * Finds the retained row for one interactable object, allocating a free row on a first
 * observation. At capacity the least recently observed row is recycled: a mission streams more
 * objects than the table holds, and a later-authored object such as a door crystal must still
 * publish its levels rather than silently vanish. Recycling drops that row's level history, so the
 * next report republishes as a baseline.
 */
[[nodiscard]] ObjectInteractionObservation*
find_object_row(RuntimeInstance& instance, const host::SenseObservation& observation) noexcept {
    ObjectInteractionObservation* spare = nullptr;
    ObjectInteractionObservation* oldest = nullptr;
    for (ObjectInteractionObservation& retained : instance.objectInteractionObservations) {
        if (retained.used && retained.registryKey == observation.key.registryKey
            && retained.objectTag == observation.key.objectTag
            && retained.slotIndex == observation.key.slotIndex) {
            retained.sequence = observation.sequence;
            return &retained;
        }
        if (!retained.used) {
            if (spare == nullptr) {
                spare = &retained;
            }
        } else if (oldest == nullptr || retained.sequence < oldest->sequence) {
            oldest = &retained;
        }
    }
    ObjectInteractionObservation* const row = spare != nullptr ? spare : oldest;
    if (row == nullptr) {
        return nullptr;
    }
    *row = {};
    row->used = true;
    row->sequence = observation.sequence;
    row->registryKey = observation.key.registryKey;
    row->objectTag = observation.key.objectTag;
    row->slotIndex = observation.key.slotIndex;
    return row;
}

/** Raises object state and accepted interaction events per interactable object. */
void push_object_interaction_edges(RuntimeInstance& instance,
                                   const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(
                observation, sense, format::kObjectSlotType, format::kObjectSenseSchema)) {
            continue;
        }
        ObjectInteractionObservation* const slot = find_object_row(instance, observation);
        if (slot == nullptr) {
            continue;
        }
        const ObjectInteractionLevel before = slot->level;
        const bool interacted = update_object_interaction(
            slot->level, observation_values(observation, sense), observation.key.schemaRow);
        const ObjectInteractionLevel& level = slot->level;
        host::Event event = sense_edge_event(instance, observation);
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
        const bool stateChanged =
            level.generationKnown && level.generation > 0 && level.stateKnown
            && (!before.stateKnown || before.generation != level.generation
                || before.present != level.present
                || before.interactionOpen != level.interactionOpen
                || before.ownerKnown != level.ownerKnown || before.hasOwner != level.hasOwner
                || before.ownerKey != level.ownerKey || before.entryIndex != level.entryIndex
                || before.spawnMask != level.spawnMask);
        if (stateChanged) {
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
        if (interacted) {
            event.kind = host::EventKind::objectInteracted;
            push_script_event(instance, event);
        }
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
        GhostObservation* const slot = find_slot_row(instance.ghostObservations, observation.key);
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

/** Hands the VM the retained Ghost-link levels, so a callback can read one it did not receive. */
void publish_ghost_levels(RuntimeInstance& instance) noexcept {
    std::array<GhostLinkRow, kGhostObservationCapacity> rows{};
    std::size_t count = 0;
    for (const GhostObservation& observation : instance.ghostObservations) {
        if (observation.used) {
            rows[count++] = {observation.level,
                             observation.registryKey,
                             observation.objectTag,
                             observation.slotIndex};
        }
    }
    lua_vm::publish_ghost_levels(instance.vm, std::span(rows).first(count));
}

/**
 * Raises the squad events derived from one msg 6 body.
 * The client publishes levels, so a first sighting only records them. A slot count that rose is the
 * only spawn signal, and the alive count falling is the only death signal.
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
        if (!observe_population(instance, observation, body)) {
            return;
        }
        // A root-absent delta carries no squad state and cannot replace the previous levels.
        if (std::none_of(body.begin(), body.end(), [root](const sense_values::DecodedValue& value) {
                return value.schemaRow == root;
            })) {
            continue;
        }
        SquadObservation* const squad = find_squad(instance, observation.key);
        if (squad == nullptr) {
            continue;
        }
        if (observation.sourceGeneration == 0
            || observation.sourceGeneration != instance.view.activityClientGeneration) {
            continue;
        }
        const bool hadPrevious = squad->epoch.used;
        const bool sameSource =
            hadPrevious && squad->epoch.sourceGeneration == observation.sourceGeneration;
        if (observation.sequence == 0
            || (sameSource && observation.sequence <= squad->epoch.sequence)) {
            continue;
        }
        if (sameSource && squad->hasReportCounter && observation.hasGeneration
            && observation.generationPlusOne == squad->reportCounter) {
            continue;
        }
        // After a Sense rebase the client re-sends already delivered counters, often with a fuller
        // delta against an older baseline. A replay repeats known state; treating it as a counter
        // reset tells the scripts the squad lost its registration and spawn echo. A native
        // re-registration restarts at one and still reconciles below.
        if (sameSource && squad->hasReportCounter && observation.hasGeneration
            && observation.generationPlusOne > 1
            && observation.generationPlusOne < squad->reportCounter) {
            continue;
        }
        // Retained levels only bridge a proven consecutive report. A gap, wrap or re-registration
        // reconciles from this body alone.
        const bool consecutive = sameSource && squad->hasReportCounter && observation.hasGeneration
                                 && squad->reportCounter != UINT32_MAX
                                 && observation.generationPlusOne == squad->reportCounter + 1;

        const sense_values::DecodedValue* const generationValue =
            sense_value(body, root, kSquadSpawnGenerationOrdinal);
        const bool bodyHasGeneration = generationValue != nullptr && generationValue->present
                                       && generationValue->signedValue >= 0
                                       && generationValue->signedValue <= kMaximumCounter;
        std::int32_t spawnGeneration =
            bodyHasGeneration ? static_cast<std::int32_t>(generationValue->signedValue) : 0;
        bool hasSpawnGeneration = bodyHasGeneration;
        if (!bodyHasGeneration && consecutive) {
            spawnGeneration = squad->spawnGeneration;
            hasSpawnGeneration = squad->hasSpawnGeneration;
        }
        const bool lifetimeChanged =
            bodyHasGeneration
            && (!squad->hasSpawnGeneration || squad->spawnGeneration != spawnGeneration);
        const bool sameLifetime = consecutive && !lifetimeChanged;
        if (!sameLifetime) {
            squad->objectiveCosts = {};
        }
        const bool costsChanged = update_squad_objective_costs(squad->objectiveCosts, body, root);

        std::int32_t alive = sameLifetime ? squad->aliveCount : 0;
        bool hasAlive = read_squad_alive(body, root, alive);
        if (!hasAlive && sameLifetime) {
            hasAlive = squad->hasAlive;
        }
        const sense_values::DecodedValue* const removalValue =
            sense_value(body, root, kSquadRemovalOrdinal);
        bool hasRemoval = removalValue != nullptr && removalValue->present;
        bool removal = hasRemoval && removalValue->unsignedValue != 0;
        if (!hasRemoval && sameLifetime) {
            removal = squad->removalFlag;
            hasRemoval = squad->hasRemoval;
        }
        auto counts = sameLifetime ? squad->slotCounts : decltype(squad->slotCounts){};
        auto countLength = sameLifetime ? squad->slotCountLength : std::uint8_t{};
        std::array<std::int32_t, host::kSquadSlotCapacity> incomingCounts{};
        const auto incomingLength = read_squad_created_counts(body, incomingCounts);
        if (incomingLength != 0) {
            counts = incomingCounts;
            countLength = incomingLength;
        }

        auto transition = observation_epoch::accept(squad->epoch,
                                                    instance.view.activityClientGeneration,
                                                    observation.sourceGeneration,
                                                    observation.sequence,
                                                    static_cast<std::uint32_t>(spawnGeneration),
                                                    hasSpawnGeneration && spawnGeneration > 0);
        const bool counterReset = hadPrevious && !consecutive;
        if (transition != observation_epoch::Transition::discard
            && (counterReset || !squad->hasAlive || !hasAlive)) {
            transition = observation_epoch::Transition::baseline;
        }
        if (transition == observation_epoch::Transition::discard) {
            continue;
        }
        squad->reportCounter = observation.generationPlusOne;
        squad->hasReportCounter = observation.hasGeneration;

        const bool first = transition == observation_epoch::Transition::baseline;
        const bool wasUsed = squad->used;
        squad->used = true;
        const std::int32_t previousAlive = first ? alive : squad->aliveCount;
        const bool changed =
            costsChanged || first || squad->aliveCount != alive || squad->hasAlive != hasAlive
            || squad->removalFlag != removal || squad->hasRemoval != hasRemoval
            || squad->spawnGeneration != spawnGeneration
            || squad->hasSpawnGeneration != hasSpawnGeneration
            || squad->slotCountLength != countLength || squad->slotCounts != counts;
        if (!changed) {
            continue;
        }
        host::Event state = sense_edge_event(instance, observation);
        state.squadObjectiveCosts = squad->objectiveCosts.values;
        state.squadObjectiveCostMask = squad->objectiveCosts.known;
        state.squadObjectiveRevision = squad->objectiveCosts.revision;
        qualify_objective_costs(instance, state);
        state.squadAliveCount = alive;
        state.squadPopulationAvailable = hasAlive;
        state.squadSpawnGeneration = spawnGeneration;
        state.squadHasSpawnGeneration = hasSpawnGeneration;
        state.squadRegistrationReset = counterReset || (hadPrevious && lifetimeChanged);
        state.squadPreviousAliveCount = previousAlive;
        state.squadRemovalFlag = removal;
        state.squadHasRemovalFlag = hasRemoval;
        state.squadSlotCounts = counts;
        state.squadSlotCountLength = countLength;
        state.initialObservation = first;
        state.kind = host::EventKind::squadState;
        if (instance.programStatus == ProgramStatus::loaded) {
            push_script_event(instance, state);
        }

        for (std::uint8_t slot = 0; slot < countLength; ++slot) {
            const std::int32_t previous =
                wasUsed && !first && slot < squad->slotCountLength ? squad->slotCounts[slot] : 0;
            if (counts[slot] <= previous) {
                continue;
            }
            host::Event spawned = sense_edge_event(instance, observation);
            spawned.squadSlotOrdinal = slot;
            spawned.squadSlotValue = counts[slot];
            spawned.squadPreviousSlotValue = previous;
            spawned.kind = host::EventKind::entitySpawned;
            if (instance.programStatus == ProgramStatus::loaded) {
                push_script_event(instance, spawned);
            }
        }
        // Only consecutive reports of one spawn lifetime may prove a population decrease.
        if (!first && hasAlive && squad->hasAlive && alive < squad->aliveCount) {
            host::Event died = sense_edge_event(instance, observation);
            died.squadAliveCount = alive;
            died.squadPopulationAvailable = true;
            died.squadSpawnGeneration = spawnGeneration;
            died.squadHasSpawnGeneration = hasSpawnGeneration;
            died.squadPreviousAliveCount = squad->aliveCount;
            died.squadRemovalFlag = removal;
            died.squadHasRemovalFlag = hasRemoval;
            died.kind = host::EventKind::entityDied;
            if (instance.programStatus == ProgramStatus::loaded) {
                push_script_event(instance, died);
            }
        }
        squad->aliveCount = alive;
        squad->hasAlive = hasAlive;
        squad->removalFlag = removal;
        squad->hasRemoval = hasRemoval;
        squad->spawnGeneration = spawnGeneration;
        squad->hasSpawnGeneration = hasSpawnGeneration;
        squad->slotCounts = counts;
        squad->slotCountLength = countLength;
    }
}

/**
 * Raises one event per watched authored scene that latched complete.
 * The completion field is a latch, so only the false to true edge is a finish. A scene seen for
 * the first time records its level and raises nothing.
 */
void push_scene_edges(RuntimeInstance& instance,
                      const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (observation.key.slotType != format::kAuthoredSceneSlotType
            || observation.key.senseSchema != format::kAuthoredSceneSenseSchema
            || observation.firstValue + observation.valueCount > sense.valueCount) {
            continue;
        }
        const std::span<const sense_values::DecodedValue> body(
            &sense.values[observation.firstValue], observation.valueCount);
        const std::uint32_t root = observation.key.schemaRow;
        // An unchanged scene sends an empty delta, which must not read as a completion of false.
        if (sense_value(body, root, kSceneCompletionOrdinal) == nullptr) {
            continue;
        }
        const bool completed = sense_flag(body, root, kSceneCompletionOrdinal);
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
        if (slot == nullptr) {
            if (spare == nullptr) {
                log_line(core::log::Level::warn, &instance, "scene", "watch_capacity");
                continue;
            }
            spare->registryKey = observation.key.registryKey;
            spare->objectTag = observation.key.objectTag;
            spare->slotIndex = observation.key.slotIndex;
            spare->completed = completed;
            spare->used = true;
            continue;
        }
        const bool rising = completed && !slot->completed;
        slot->completed = completed;
        if (!rising) {
            continue;
        }
        host::Event event = sense_edge_event(instance, observation);
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
        if (observation.key.slotType != kObjectiveSlotType
            || observation.key.senseSchema != kObjectiveSenseSchema
            || observation.firstValue + observation.valueCount > sense.valueCount) {
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
                host::Event event = sense_edge_event(instance, observation);
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
