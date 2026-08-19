#include <variant>

#include "command_effects.h"

namespace sunrise::server::gameplay::physics::host::detail {
namespace {

[[nodiscard]] const world::ActorSnapshot* find_actor(std::span<const world::ActorSnapshot> actors,
                                                     world::ActorKey actor) noexcept {
    for (const world::ActorSnapshot& candidate : actors) {
        if (world::equal_actor(candidate.key, actor)) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] bool same_blueprint(world::BlueprintRef left, world::BlueprintRef right) noexcept {
    return left.id == right.id && left.version == right.version;
}

/** @return Canonical combat setup event. */
[[nodiscard]] world::CommittedEvent command_event(const world::HostExecutionContext& context,
                                                  const world::HostCommand& command,
                                                  const world::ActorSnapshot& actor,
                                                  world::CommittedEventKind kind,
                                                  std::uint64_t value) noexcept {
    world::CommittedEvent event{};
    event.actor = actor.key;
    event.transform = actor.transform;
    event.commandId = command.header.commandId;
    event.tick = context.tick;
    event.actorRevision = actor.revision;
    event.authorityGeneration = actor.authority.generation;
    event.value = value;
    event.kind = kind;
    return event;
}

/** Configures one actor against immutable combat build data. */
[[nodiscard]] bool configure_combat(BubbleHost::Storage::WorldSlot& slot,
                                    const world::HostExecutionContext& context,
                                    const world::HostCommand& command,
                                    std::span<const world::ActorSnapshot> actors,
                                    EventWriter& writer) noexcept {
    const auto& configure = std::get<world::ConfigureCombatCommand>(command.payload);
    const world::ActorSnapshot* actor = find_actor(actors, configure.actor);
    if (actor == nullptr) {
        return false;
    }
    const CombatProfile* profile = nullptr;
    for (const BubbleHost::Storage::CombatProfileSlot& candidate : slot.combatProfiles) {
        if (candidate.occupied && same_blueprint(candidate.profile.profile, configure.combatProfile)
            && candidate.profile.policyOwnerId == command.header.policyOwnerId) {
            profile = &candidate.profile;
            break;
        }
    }
    if (profile == nullptr) {
        return false;
    }

    BubbleHost::Storage::CombatantBinding* freeSlot = nullptr;
    for (BubbleHost::Storage::CombatantBinding& binding : slot.combatants) {
        if (binding.occupied && world::equal_actor(binding.actor, configure.actor)) {
            if (binding.profile.id != 0
                && !same_blueprint(binding.profile, configure.combatProfile)) {
                return false;
            }
            binding.profile = configure.combatProfile;
            return writer.append(command_event(context,
                                               command,
                                               *actor,
                                               world::CommittedEventKind::combatConfigured,
                                               configure.combatProfile.id));
        }
        if (!binding.occupied && freeSlot == nullptr) {
            freeSlot = &binding;
        }
    }
    if (freeSlot == nullptr) {
        return false;
    }

    mechanics::CombatantDefinition definition{};
    definition.actor = {configure.actor.id, configure.actor.generation};
    definition.policyOwnerId = profile->policyOwnerId;
    definition.hostileFactionMask = profile->hostileFactionMask;
    definition.bubble = actor->bubble;
    definition.maximumHealth = profile->maximumHealth;
    definition.maximumShield = profile->maximumShield;
    definition.faction = profile->faction;
    definition.canDealDamage = profile->canDealDamage;
    definition.canReceiveDamage = profile->canReceiveDamage;
    mechanics::CombatantHandle combatant{};
    if (slot.combat.create_combatant(definition, combatant) != mechanics::CombatResult::applied) {
        return false;
    }
    freeSlot->actor = configure.actor;
    freeSlot->profile = configure.combatProfile;
    freeSlot->combatant = combatant;
    freeSlot->occupied = true;
    return writer.append(command_event(context,
                                       command,
                                       *actor,
                                       world::CommittedEventKind::combatConfigured,
                                       configure.combatProfile.id));
}

} // namespace

/** Applies one combat configuration on the command-commit boundary. */
bool execute_combat_setup_command(BubbleHost::Storage::WorldSlot& slot,
                                  const world::HostExecutionContext& context,
                                  const world::HostCommand& command,
                                  std::span<const world::ActorSnapshot> actors,
                                  EventWriter& writer) noexcept {
    return world::command_kind(command) == world::HostCommandKind::configureCombat
           && configure_combat(slot, context, command, actors, writer);
}

} // namespace sunrise::server::gameplay::physics::host::detail
