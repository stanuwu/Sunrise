#include <array>
#include <cstdio>
#include <span>

#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

/**
 * Retains each combatant under its complete authored slot identity.
 * @param instance Bound mission instance.
 * @param key Exact accepted Sense source.
 * @return The matching or newly allocated observation, or null at capacity.
 */
[[nodiscard]] CombatantDamageObservation*
find_combatant_damage(RuntimeInstance& instance, const host::SenseObservationKey& key) noexcept {
    CombatantDamageObservation* spare = nullptr;
    for (auto& row : instance.combatantDamageObservations) {
        if (row.used && row.registryKey == key.registryKey && row.objectTag == key.objectTag
            && row.slotIndex == key.slotIndex) {
            return &row;
        }
        if (!row.used && spare == nullptr) {
            spare = &row;
        }
    }
    if (spare != nullptr) {
        spare->registryKey = key.registryKey;
        spare->objectTag = key.objectTag;
        spare->slotIndex = key.slotIndex;
        spare->used = true;
    }
    return spare;
}

/**
 * Raises one damageState event for a level that just changed, and logs it.
 * @param key Row identity; an entity-sourced row has no object tag or Sense schema to publish.
 * @param source "sense" or "entity", for the log only.
 */
void push_damage_state(RuntimeInstance& instance,
                       const host::SenseObservationKey& key,
                       const CombatantDamageLevel& level,
                       std::uint64_t sequence,
                       std::uint64_t tick,
                       const char* source) noexcept {
    host::Event event{};
    event.kind = host::EventKind::damageState;
    event.binding = instance.view.binding;
    event.sequence = sequence;
    event.tick = tick;
    event.sourceGeneration = instance.view.activityClientGeneration;
    event.missionSequence = instance.lastMissionSequence;
    event.firstRegistryKey = key.registryKey;
    event.firstSlotIndex = key.slotIndex;
    event.firstSlotType = key.slotType;
    event.slotObjectTag = key.senseSchema != 0 ? key.objectTag : 0;
    event.slotSenseSchema = key.senseSchema;
    event.damageHealth = level.primary;
    event.damageShield = level.secondary;
    event.damageRevision = level.revision;
    std::array<char, 128> details{};
    const int written = std::snprintf(details.data(),
                                      details.size(),
                                      "registry=%08X type=%u slot=%u revision=%d primary=%.4f "
                                      "secondary=%.4f source=%s",
                                      key.registryKey,
                                      static_cast<unsigned>(key.slotType),
                                      static_cast<unsigned>(key.slotIndex),
                                      level.revision,
                                      static_cast<double>(level.primary),
                                      static_cast<double>(level.secondary),
                                      source);
    if (written > 0 && static_cast<std::size_t>(written) < details.size()) {
        log_line(core::log::Level::debug,
                 &instance,
                 "combatant_damage",
                 "changed",
                 {details.data(), static_cast<std::size_t>(written)});
    }
    push_script_event(instance, event);
}

} // namespace

/**
 * Raises damage-state observations without inferring actor attachment or provocation.
 * @param instance Bound mission instance retaining prior combatant levels.
 * @param sense Accepted observations for the current client generation.
 */
void push_combatant_damage_edges(RuntimeInstance& instance,
                                 const host::SenseObservationSnapshot& sense) noexcept {
    if (sense.sourceGeneration != instance.view.activityClientGeneration
        || sense.observationCount > sense.observations.size()
        || sense.valueCount > sense.values.size()) {
        return;
    }
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const auto& observation = sense.observations[index];
        if (observation.sourceGeneration != sense.sourceGeneration
            || observation.key.slotType != kCombatantDamageSlotType
            || observation.key.senseSchema != kCombatantDamageSenseSchema
            || observation.firstValue > sense.valueCount
            || observation.valueCount > sense.valueCount - observation.firstValue) {
            continue;
        }
        auto* const row = find_combatant_damage(instance, observation.key);
        if (row == nullptr) {
            log_line(core::log::Level::warn, &instance, "combatant_damage", "watch_capacity");
            continue;
        }
        const auto body =
            std::span(sense.values).subspan(observation.firstValue, observation.valueCount);
        if (!update_combatant_damage(row->level, body, observation.key.schemaRow)) {
            continue;
        }
        push_damage_state(
            instance, observation.key, row->level, observation.sequence, observation.tick, "sense");
    }
}

/**
 * Raises a damage state from an entity's replicated damage component when its levels change.
 * Entity rows share the Sense watch table; they have no object tag, so the slot type stands in its
 * place in the row key.
 */
void push_entity_damage(RuntimeInstance& instance,
                        std::uint32_t registryKey,
                        std::uint8_t slotType,
                        std::uint16_t slotIndex,
                        float primary,
                        float secondary,
                        std::uint64_t tick) noexcept {
    host::SenseObservationKey key{};
    key.registryKey = registryKey;
    key.objectTag = slotType;
    key.senseSchema = 0;
    key.slotIndex = slotIndex;
    key.slotType = slotType;
    auto* const row = find_combatant_damage(instance, key);
    if (row == nullptr) {
        log_line(core::log::Level::warn, &instance, "combatant_damage", "watch_capacity");
        return;
    }
    if (row->level.observed && row->level.primary == primary
        && row->level.secondary == secondary) {
        return;
    }
    row->level.primary = primary;
    row->level.secondary = secondary;
    row->level.observed = true;
    ++row->level.revision;
    push_damage_state(
        instance, key, row->level, instance.missionStateRevision, tick, "entity");
}

} // namespace sunrise::server::activity::mission
