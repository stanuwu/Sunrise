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

/** @return A squad edge carrying the squad's retained spawn generation. */
[[nodiscard]] host::Event squad_edge_event(const RuntimeInstance& instance,
                                           const host::SenseObservation& observation,
                                           const SquadObservation& squad) noexcept {
    host::Event event = sense_edge_event(instance, observation);
    event.squadSpawnGeneration = squad.spawnGeneration;
    event.squadHasSpawnGeneration = squad.hasSpawnGeneration;
    return event;
}

/** @return The retained record for one squad, allocating on a first observation. */
[[nodiscard]] SquadObservation* find_squad(RuntimeInstance& instance,
                                           const host::SenseObservationKey& key) noexcept {
    SquadObservation* spare = nullptr;
    for (SquadObservation& retained : instance.squadObservations) {
        if (retained.used && retained.registryKey == key.registryKey
            && retained.objectTag == key.objectTag && retained.slotIndex == key.slotIndex) {
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
    return spare;
}

/**
 * Raises one triggerState event for a report its tracker does not drop. The event only informs;
 * entered and exited follow the level alone.
 * @param event The report's level, already filled in.
 */
void push_trigger_state(RuntimeInstance& instance,
                        const host::SenseObservationSnapshot& sense,
                        const host::SenseObservation& observation,
                        TriggerOccupancy& slot,
                        host::Event event) noexcept {
    trigger_observation::Snapshot level{};
    const bool valid =
        observation.valueCount <= sense.valueCount - observation.firstValue
        && trigger_observation::read(std::span(sense.values.data(), sense.valueCount)
                                         .subspan(observation.firstValue, observation.valueCount),
                                     level);
    const auto result = slot.tracker.observe(valid ? &level : nullptr,
                                             instance.view.activityClientGeneration,
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
        push_trigger_state(instance, sense, observation, *slot, event);
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

/** Raises object state and accepted interaction events per interactable object. */
void push_object_interaction_edges(RuntimeInstance& instance,
                                   const host::SenseObservationSnapshot& sense) noexcept {
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const host::SenseObservation& observation = sense.observations[index];
        if (!observation_of(
                observation, sense, format::kObjectSlotType, format::kObjectSenseSchema)) {
            continue;
        }
        ObjectInteractionObservation* const slot =
            find_slot_row(instance.objectInteractionObservations, observation.key);
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
                || before.ownerKey != level.ownerKey);
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
        SquadObservation* const squad = find_squad(instance, observation.key);
        if (squad == nullptr) {
            continue;
        }
        const bool costsChanged = update_squad_objective_costs(squad->objectiveCosts, body, root);
        // Cost-only deltas retain combat counts; they must never manufacture a death.
        std::int32_t alive = squad->aliveCount;
        const bool hasAlive = read_squad_alive(body, root, alive);
        const bool removal = sense_flag(body, root, kSquadRemovalOrdinal);
        auto counts = squad->slotCounts;
        auto countLength = squad->slotCountLength;
        std::array<std::int32_t, host::kSquadSlotCapacity> incomingCounts{};
        const auto incomingLength = read_squad_created_counts(body, incomingCounts);
        if (incomingLength != 0) {
            counts = incomingCounts;
            countLength = incomingLength;
        }
        if (!hasAlive && !costsChanged && incomingLength == 0) {
            continue;
        }
        const bool first = !squad->used;
        squad->used = true;
        squad->populationKnown = squad->populationKnown || hasAlive;
        // An absent spawn generation keeps the last one reported, like the other retained levels.
        const sense_values::DecodedValue* const spawn =
            sense_value(body, root, kSquadSpawnGenerationOrdinal);
        if (spawn != nullptr && spawn->present && spawn->signedValue >= 0
            && spawn->signedValue <= kMaximumCounter) {
            squad->spawnGeneration = static_cast<std::int32_t>(spawn->signedValue);
            squad->hasSpawnGeneration = true;
        }
        const std::int32_t previousAlive = first ? 0 : squad->aliveCount;
        const bool changed =
            costsChanged || first || squad->aliveCount != alive || squad->removalFlag != removal
            || squad->slotCountLength != countLength || squad->slotCounts != counts;
        if (!changed) {
            continue;
        }
        host::Event state = squad_edge_event(instance, observation, *squad);
        state.squadObjectiveCosts = squad->objectiveCosts.values;
        state.squadObjectiveCostMask = squad->objectiveCosts.known;
        state.squadObjectiveRevision = squad->objectiveCosts.revision;
        qualify_objective_costs(instance, state);
        state.squadAliveCount = alive;
        state.squadPreviousAliveCount = previousAlive;
        state.squadPopulationAvailable = squad->populationKnown;
        state.squadRemovalFlag = removal;
        state.squadSlotCounts = counts;
        state.squadSlotCountLength = countLength;
        state.initialObservation = first;
        state.kind = host::EventKind::squadState;
        if (instance.programStatus == ProgramStatus::loaded) {
            push_script_event(instance, state);
        }

        for (std::uint8_t slot = 0; slot < countLength; ++slot) {
            const std::int32_t previous =
                !first && slot < squad->slotCountLength ? squad->slotCounts[slot] : 0;
            if (counts[slot] <= previous) {
                continue;
            }
            host::Event spawned = squad_edge_event(instance, observation, *squad);
            spawned.squadSlotOrdinal = slot;
            spawned.squadSlotValue = counts[slot];
            spawned.squadPreviousSlotValue = previous;
            spawned.kind = host::EventKind::entitySpawned;
            if (instance.programStatus == ProgramStatus::loaded) {
                push_script_event(instance, spawned);
            }
        }
        if (!first && alive < squad->aliveCount) {
            host::Event died = squad_edge_event(instance, observation, *squad);
            died.squadAliveCount = alive;
            died.squadPreviousAliveCount = squad->aliveCount;
            died.squadRemovalFlag = removal;
            died.kind = host::EventKind::entityDied;
            if (instance.programStatus == ProgramStatus::loaded) {
                push_script_event(instance, died);
            }
        }
        squad->aliveCount = alive;
        squad->removalFlag = removal;
        squad->slotCounts = counts;
        squad->slotCountLength = countLength;
    }
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
        host::Event event = sense_edge_event(instance, observation);
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
            || observation.firstValue + observation.valueCount > sense.valueCount) {
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
        if (slot == nullptr) {
            if (spare == nullptr) {
                log_line(core::log::Level::warn, &instance, "scene", "watch_capacity");
                continue;
            }
            *spare = {};
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
        host::Event event = sense_edge_event(instance, observation);
        event.sceneActivationToken = sense_number(body, root, kSceneTokenOrdinal);
        event.kind = host::EventKind::sceneFinished;
        push_script_event(instance, event);
    }
}

} // namespace sunrise::server::activity::mission
