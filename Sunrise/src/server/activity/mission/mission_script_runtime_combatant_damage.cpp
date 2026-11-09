#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdio>
#include <span>

#include "../../gameplay/entity_identities.h"
#include "mission_script_entity_damage.h"
#include "mission_script_runtime_internal.h"

namespace sunrise::server::activity::mission {
namespace {

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

namespace {

/** What one poll of the entity registry carries through the walk and out of the lock. */
struct EntityDamageScan final {
    RuntimeInstance* instance{};
    /** Link whose entity source this walk reads. */
    std::uint64_t sourceGeneration{};
    std::array<state::gameplay::entity_identity::Identity, kSquadObservationCapacity> changed{};
    /** Network slots this source held a row for before the walk. */
    std::bitset<state::gameplay::entity_identity::kSlotCapacity> retainedSlots{};
    std::size_t changedCount{};
    std::size_t overflow{};
    /** Set when the route is the only live link, so every pin falls back to it. */
    bool routeOnly{};
};

/**
 * Retains each entity under its source's network slot; a reused slot replaces the earlier
 * incarnation.
 * @return The matching or free row, or null at capacity.
 */
[[nodiscard]] EntityDamageObservation*
find_entity_damage(RuntimeInstance& instance,
                   std::uint64_t sourceGeneration,
                   state::gameplay::entity_identity::Token token) noexcept {
    EntityDamageObservation* spare = nullptr;
    for (auto& row : instance.entitySourceDamageObservations) {
        if (row.used && row.sourceGeneration == sourceGeneration && row.token.slot == token.slot) {
            return &row;
        }
        if (!row.used && spare == nullptr) {
            spare = &row;
        }
    }
    return spare;
}

/**
 * @return The link whose entity source speaks for one combatant, by the same chain its Sense
 * reports are admitted by. Another link's copy of it is not its simulation.
 */
[[nodiscard]] std::uint64_t entity_damage_generation(
    const RuntimeInstance& instance,
    const state::gameplay::entity_identity::ActorSourceReference& actorSource) noexcept {
    return simulating_link(instance,
                           {.registryKey = actorSource.key,
                            .slotIndex = actorSource.index,
                            .slotType = actorSource.type});
}

/** @return True when the route is the only live link, so every pin falls back to it. */
[[nodiscard]] bool route_only(const RuntimeInstance& instance) noexcept {
    const auto live = live_links(instance);
    return std::all_of(live.begin(), live.end(), [&instance](std::uint64_t link) noexcept {
        return link == instance.view.activityClientGeneration;
    });
}

/**
 * @return True when one link's copy of a combatant is its simulation. The walk runs under the
 * registry lock for every visited row, so with the route the only live link, where every pin falls
 * back to the route, the pin walk is skipped.
 */
[[nodiscard]] bool
simulated_copy(const RuntimeInstance& instance,
               bool routeOnly,
               std::uint64_t sourceGeneration,
               const state::gameplay::entity_identity::ActorSourceReference& actorSource) noexcept {
    return routeOnly ? sourceGeneration == instance.view.activityClientGeneration
                     : entity_damage_generation(instance, actorSource) == sourceGeneration;
}

/**
 * Collects one entity whose pools differ from the last published ones, or whose publication was
 * refused at dispatch. Only the copy of the link that simulates a combatant holds a row: every
 * link's source also lists replica copies, and rows held for them would take the table from the
 * copies that are published. A copy that is not the simulation frees its row; once its link
 * simulates the combatant, its first visit finds no row and publishes it. It runs under the
 * registry lock, so it only compares and copies; events are raised after the walk.
 */
[[nodiscard]] bool
observe_entity_damage(void* context,
                      const state::gameplay::entity_identity::Identity& row) noexcept {
    namespace identities = state::gameplay::entity_identity;
    auto& scan = *static_cast<EntityDamageScan*>(context);
    // A row stays while the registry lists its entity, even one that reports no pools now.
    if (row.known && row.present && row.token.slot < scan.retainedSlots.size()
        && scan.retainedSlots.test(row.token.slot)) {
        for (auto& retained : scan.instance->entitySourceDamageObservations) {
            if (retained.used && retained.sourceGeneration == scan.sourceGeneration
                && retained.token == row.token) {
                retained.listed = true;
            }
        }
    }
    // Type zero is the entity record, the same record the retirement scan reads.
    if (!row.known || !row.present || row.conflicted || row.type != 0 || !row.actorSource.known
        || !row.actorSource.present || row.actorSource.type != kCombatantDamageSlotType
        || (!row.damage.present[identities::kHealthPoolIndex]
            && !row.damage.present[identities::kShieldPoolIndex])) {
        return true;
    }
    EntityDamageObservation* const retained =
        find_entity_damage(*scan.instance, scan.sourceGeneration, row.token);
    if (!simulated_copy(*scan.instance, scan.routeOnly, scan.sourceGeneration, row.actorSource)) {
        if (retained != nullptr && retained->used) {
            *retained = {};
        }
        return true;
    }
    if (retained == nullptr) {
        ++scan.overflow;
        return true;
    }
    retained->listed = true;
    const bool unchanged = retained->used && retained->token == row.token
                           && retained->allocationSequence == row.allocationSequence
                           && retained->allocationDomain == row.allocationDomain
                           && retained->actorSource == row.actorSource
                           && retained->damage == row.damage;
    if (unchanged && !retained->withheld) {
        return true;
    }
    retained->token = row.token;
    retained->actorSource = row.actorSource;
    retained->damage = row.damage;
    retained->allocationDomain = row.allocationDomain;
    retained->allocationSequence = row.allocationSequence;
    retained->allocationEpoch = row.allocationEpoch;
    retained->sourceGeneration = scan.sourceGeneration;
    retained->withheld = false;
    retained->used = true;
    // Every changed row owns a retained row, so the retained capacity bounds this array too.
    if (scan.changedCount < scan.changed.size()) {
        scan.changed[scan.changedCount++] = row;
    }
    return true;
}

/** @return The one authored combatant this actor source names in the world, or null. */
[[nodiscard]] const state::build_data::scriptables::Object* unique_combatant(
    const state::build_data::scriptables::Snapshot& world,
    const state::gameplay::entity_identity::ActorSourceReference& actorSource) noexcept {
    const state::build_data::scriptables::Object* object = nullptr;
    std::size_t matches = 0;
    for (const auto& candidate : world.objects) {
        if (!candidate.complete || candidate.registryKey != actorSource.key
            || candidate.firstSlot > world.slots.size()
            || candidate.slotCount > world.slots.size() - candidate.firstSlot) {
            continue;
        }
        for (const auto& slot :
             std::span(world.slots).subspan(candidate.firstSlot, candidate.slotCount)) {
            if (slot.slotIndex == actorSource.index && slot.slotType == actorSource.type) {
                object = &candidate;
                ++matches;
            }
        }
    }
    return matches == 1 ? object : nullptr;
}

/** @return The pool as a fraction of full, or the unknown level when the client has not sent it. */
[[nodiscard]] float normalised_level(const state::gameplay::entity_identity::DamagePools& pools,
                                     std::size_t pool) noexcept {
    namespace identities = state::gameplay::entity_identity;
    return pools.present[pool]
               ? static_cast<float>(pools.levels[pool])
                     / static_cast<float>(middleware::gameplay::external::kDamageLevelFull)
               : kUnknownDamageLevel;
}

/** Forgets one link's published pools and its capacity warning; a replacement source starts over.
 */
void reset_entity_damage(RuntimeInstance& instance, std::uint64_t sourceGeneration) noexcept {
    for (auto& row : instance.entitySourceDamageObservations) {
        if (row.used && row.sourceGeneration == sourceGeneration) {
            row = {};
        }
    }
    for (auto& retained : instance.damageEntitySources) {
        if (retained.source.activityClientGeneration == sourceGeneration) {
            retained = {};
        }
    }
}

/** @return The source retained for one link, or null when none is. */
[[nodiscard]] EntityDamageSource* retained_entity_source(RuntimeInstance& instance,
                                                         std::uint64_t sourceGeneration) noexcept {
    for (auto& retained : instance.damageEntitySources) {
        if (retained.source.activityClientGeneration == sourceGeneration) {
            return &retained;
        }
    }
    return nullptr;
}

/**
 * Lists the links whose entity sources one poll reads: the route, then each live link a retained
 * body is pinned to, because the client holding a target's body simulates its entities.
 * @return The number of generations written.
 */
[[nodiscard]] std::size_t entity_damage_generations(const RuntimeInstance& instance,
                                                    std::span<std::uint64_t> output) noexcept {
    std::size_t count = 0;
    const auto add = [&](std::uint64_t generation) noexcept {
        const auto written = output.begin() + static_cast<std::ptrdiff_t>(count);
        if (generation != 0 && count < output.size()
            && std::find(output.begin(), written, generation) == written) {
            output[count++] = generation;
        }
    };
    add(instance.view.activityClientGeneration);
    const auto live = live_links(instance);
    for (const host::ScriptablePin& pin : pass_pins(instance)) {
        if (std::find(live.begin(), live.end(), pin.expectedActivityClientGeneration)
            != live.end()) {
            add(pin.expectedActivityClientGeneration);
        }
    }
    return count;
}

} // namespace

/**
 * Revalidates queued health against the same source and allocation. A pin can move between the poll
 * and this dispatch, which makes the copy another link's: its row is withheld, so the next poll
 * frees it, or publishes it again while its link still simulates the combatant.
 */
bool current_entity_damage(RuntimeInstance& instance, const host::Event& event) noexcept {
    namespace identities = state::gameplay::entity_identity;
    std::array<identities::Source, identities::kSourceCapacity> sources{};
    const auto count = gameplay::entity_identities::sources(
        event.binding.sessionId, event.binding.createdRevision, sources);
    std::size_t matches = 0;
    for (std::size_t index = 0; index < count && index < sources.size(); ++index) {
        if (sources[index].activityClientGeneration == event.sourceGeneration) {
            if (sources[index] != event.damageEntitySource) {
                return false;
            }
            ++matches;
        }
    }
    if (count > sources.size() || matches != 1) {
        return false;
    }
    identities::Identity current{};
    if (!state::activity::binding_matches(event.binding)
        || event.damageEntitySource.activitySessionId != event.binding.sessionId
        || event.damageEntitySource.activityRevision != event.binding.createdRevision
        || event.damageEntitySource.activityClientGeneration != event.sourceGeneration
        || gameplay::entity_identities::lookup(
               event.damageEntitySource, event.damageEntityLifetime.token, current)
               != identities::Result::unchanged
        || !matches_entity_damage(event, current) || !copy_pins(instance)) {
        return false;
    }
    if (entity_damage_generation(instance, current.actorSource) == event.sourceGeneration) {
        return true;
    }
    for (auto& row : instance.entitySourceDamageObservations) {
        if (row.used && row.sourceGeneration == event.sourceGeneration
            && row.token == event.damageEntityLifetime.token) {
            row.withheld = true;
        }
    }
    return false;
}

/** Publishes changed pools of one link's unique admitted entity source. */
static void
push_source_damage_edges(RuntimeInstance& instance,
                         const state::build_data::scriptables::Snapshot& world,
                         std::uint64_t generation,
                         std::span<const state::gameplay::entity_identity::Source> sources,
                         std::uint64_t now) noexcept {
    namespace identities = state::gameplay::entity_identity;
    identities::Source source{};
    std::size_t matches = 0;
    for (const identities::Source& candidate : sources) {
        if (candidate.activityClientGeneration == generation) {
            source = candidate;
            ++matches;
        }
    }
    EntityDamageSource* retained = retained_entity_source(instance, generation);
    if (matches != 1) {
        if (retained != nullptr) {
            reset_entity_damage(instance, generation);
        }
        return;
    }
    if (retained == nullptr || retained->source != source) {
        reset_entity_damage(instance, generation);
        retained = retained_entity_source(instance, 0);
        if (retained == nullptr) {
            return;
        }
        retained->source = source;
    }
    EntityDamageScan scan{};
    scan.instance = &instance;
    scan.sourceGeneration = generation;
    scan.routeOnly = route_only(instance);
    for (auto& row : instance.entitySourceDamageObservations) {
        if (row.used && row.sourceGeneration == generation) {
            row.listed = false;
            if (row.token.slot < scan.retainedSlots.size()) {
                scan.retainedSlots.set(row.token.slot);
            }
        }
    }
    if (gameplay::entity_identities::visit_source(source, observe_entity_damage, &scan)
        != identities::Result::unchanged) {
        return;
    }
    // An entity the registry no longer lists is simulated by no one, so its row frees for others.
    for (auto& row : instance.entitySourceDamageObservations) {
        if (row.used && row.sourceGeneration == generation && !row.listed) {
            row = {};
        }
    }
    if (scan.overflow != 0 && !retained->atCapacity) {
        retained->atCapacity = true;
        log_line(core::log::Level::warn, &instance, "entity_damage", "watch_capacity");
    }
    for (const identities::Identity& row : std::span(scan.changed).first(scan.changedCount)) {
        const auto* const object = unique_combatant(world, row.actorSource);
        if (object == nullptr) {
            continue;
        }
        auto event = state_edge_event(instance);
        // The event keeps its entity source's link, which the dispatch revalidates.
        event.sourceGeneration = generation;
        event.kind = host::EventKind::damageState;
        event.tick = now;
        event.sequence = row.revision;
        event.firstRegistryKey = row.actorSource.key;
        event.firstSlotIndex = row.actorSource.index;
        event.firstSlotType = row.actorSource.type;
        event.slotObjectTag = object->objectTag;
        event.slotSenseSchema = kCombatantDamageSenseSchema;
        event.damageFromEntity = true;
        event.damageEntitySource = source;
        event.damageEntityLifetime = {
            row.token, row.allocationSequence, row.allocationEpoch, row.allocationDomain};
        event.damageEntityPools = row.damage;
        event.damageHealth = normalised_level(row.damage, identities::kHealthPoolIndex);
        event.damageShield = normalised_level(row.damage, identities::kShieldPoolIndex);
        // The entity channel carries no Sense spawn revision. Scripts use the explicit entity
        // allocation fields instead of correlating unrelated revision counters.
        push_script_event(instance, event);
    }
}

/** Polls the entity source of every link that simulates a target, forgetting links that left. */
static void push_entity_damage_edges(RuntimeInstance& instance,
                                     const state::build_data::scriptables::Snapshot& world,
                                     std::uint64_t now) noexcept {
    namespace identities = state::gameplay::entity_identity;
    if (now < instance.nextEntityDamageTick) {
        return;
    }
    instance.nextEntityDamageTick = now + kEntityDamagePollIntervalMs;
    if (!state::activity::binding_matches(instance.view.binding) || !copy_pins(instance)) {
        return;
    }
    std::array<identities::Source, identities::kSourceCapacity> sources{};
    const auto count = gameplay::entity_identities::sources(
        instance.view.binding.sessionId, instance.view.binding.createdRevision, sources);
    if (count > sources.size()) {
        return;
    }
    std::array<std::uint64_t, identities::kSourceCapacity> generations{};
    const auto polled =
        std::span(generations).first(entity_damage_generations(instance, generations));
    for (const EntityDamageSource& retained : instance.damageEntitySources) {
        const std::uint64_t generation = retained.source.activityClientGeneration;
        if (generation != 0
            && std::find(polled.begin(), polled.end(), generation) == polled.end()) {
            reset_entity_damage(instance, generation);
        }
    }
    // A re-pin turns rows into replica copies. They free before any source is walked, so the copies
    // that are the simulation now find the rows whichever source is walked first.
    const bool routeOnly = route_only(instance);
    for (auto& row : instance.entitySourceDamageObservations) {
        if (row.used
            && !simulated_copy(instance, routeOnly, row.sourceGeneration, row.actorSource)) {
            row = {};
        }
    }
    for (const std::uint64_t generation : polled) {
        push_source_damage_edges(instance, world, generation, std::span(sources).first(count), now);
    }
}

void push_entity_damage_edges(RuntimeInstance& instance, std::uint64_t now) noexcept {
    if (const auto* world = instance.worldView.snapshot()) {
        push_entity_damage_edges(instance, *world, now);
    }
}

/**
 * Raises damage-state observations without inferring actor attachment or provocation.
 * @param instance Bound mission instance retaining prior combatant levels.
 * @param sense Accepted observations of one link; only the link simulating a combatant reports it.
 */
void push_combatant_damage_edges(RuntimeInstance& instance,
                                 const host::SenseObservationSnapshot& sense) noexcept {
    if (sense.observationCount > sense.observations.size()
        || sense.valueCount > sense.values.size()) {
        return;
    }
    for (std::size_t index = 0; index < sense.observationCount; ++index) {
        const auto& observation = sense.observations[index];
        if (observation.sourceGeneration != sense.sourceGeneration
            || observation.key.slotType != kCombatantDamageSlotType
            || observation.key.senseSchema != kCombatantDamageSenseSchema
            || observation.firstValue > sense.valueCount
            || observation.valueCount > sense.valueCount - observation.firstValue
            || !admitted_report(instance, observation.key, sense.sourceGeneration)) {
            continue;
        }
        auto* const row = find_slot_row(instance.combatantDamageObservations, observation.key);
        if (row == nullptr) {
            log_line(core::log::Level::warn, &instance, "combatant_damage", "watch_capacity");
            continue;
        }
        claim_row(*row, sense.sourceGeneration, true);
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
 * Entity rows have no object tag, so the slot type fills that key field and keeps a squad and a
 * combatant with the same authored index apart.
 */
void push_entity_damage(RuntimeInstance& instance,
                        std::uint64_t sourceGeneration,
                        std::uint32_t registryKey,
                        std::uint8_t slotType,
                        std::uint16_t slotIndex,
                        float primary,
                        float secondary,
                        std::uint64_t tick) noexcept {
    // The entity poll alone publishes a combatant's own levels, read from its pinned link with
    // their lifetime. This path keeps the other keys, a combatant raised to its squad and other
    // slot types, each only from the link that simulates it, as Sense reports are admitted. Every
    // link's source also lists replica copies of targets another client simulates.
    const OutputClientRef target{
        .registryKey = registryKey, .slotIndex = slotIndex, .slotType = slotType};
    if (slotType == kCombatantDamageSlotType || sourceGeneration == 0
        || simulating_link(instance, target) != sourceGeneration) {
        return;
    }
    host::SenseObservationKey key{};
    key.registryKey = registryKey;
    key.objectTag = slotType;
    key.senseSchema = 0;
    key.slotIndex = slotIndex;
    key.slotType = slotType;
    auto* const row = find_slot_row(instance.entityDamageObservations, key);
    if (row == nullptr) {
        log_line(core::log::Level::warn, &instance, "combatant_damage", "watch_capacity");
        return;
    }
    claim_row(*row, sourceGeneration, true);
    if (row->level.observed && row->level.primary == primary && row->level.secondary == secondary) {
        return;
    }
    row->level.primary = primary;
    row->level.secondary = secondary;
    row->level.observed = true;
    ++row->level.revision;
    push_damage_state(instance, key, row->level, instance.missionStateRevision, tick, "entity");
}

} // namespace sunrise::server::activity::mission
