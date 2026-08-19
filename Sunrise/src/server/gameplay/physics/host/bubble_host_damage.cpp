#include <cmath>
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

[[nodiscard]] const BubbleHost::Storage::BodyBinding*
find_body(const BubbleHost::Storage::WorldSlot& slot, world::ActorKey actor) noexcept {
    for (const BubbleHost::Storage::BodyBinding& binding : slot.bodies) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            return &binding;
        }
    }
    return nullptr;
}

[[nodiscard]] BubbleHost::Storage::CombatantBinding*
find_combatant(BubbleHost::Storage::WorldSlot& slot, world::ActorKey actor) noexcept {
    for (BubbleHost::Storage::CombatantBinding& binding : slot.combatants) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            return &binding;
        }
    }
    return nullptr;
}

[[nodiscard]] BubbleHost::Storage::AttackProfileSlot*
find_attack(BubbleHost::Storage::WorldSlot& slot, world::BlueprintRef attack) noexcept {
    for (BubbleHost::Storage::AttackProfileSlot& binding : slot.attackProfiles) {
        if (binding.occupied && binding.profile.attack.id == attack.id
            && binding.profile.attack.version == attack.version) {
            return &binding;
        }
    }
    return nullptr;
}

[[nodiscard]] mechanics::CommandHeader
mechanics_header(const world::HostExecutionContext& context,
                 const world::HostCommand& command) noexcept {
    return {context.world.worldGeneration,
            command.header.commandId,
            context.tick,
            command.header.policyOwnerId,
            command.header.definition.version};
}

/** @return True when no blocking body precedes the target. */
[[nodiscard]] bool line_of_sight(BubbleHost::Storage::WorldSlot& slot,
                                 const world::ActorSnapshot& source,
                                 const world::ActorSnapshot& target) noexcept {
    const BubbleHost::Storage::BodyBinding* sourceBinding = find_body(slot, source.key);
    const BubbleHost::Storage::BodyBinding* targetBinding = find_body(slot, target.key);
    if (sourceBinding == nullptr || targetBinding == nullptr) {
        return false;
    }
    backend::BodyState sourceBody{};
    backend::BodyState targetBody{};
    if (!slot.physics.read_body(sourceBinding->body, sourceBody)
        || !slot.physics.read_body(targetBinding->body, targetBody)) {
        return false;
    }
    const backend::Vec3 direction{
        targetBody.spec.transform.position.x - sourceBody.spec.transform.position.x,
        targetBody.spec.transform.position.y - sourceBody.spec.transform.position.y,
        targetBody.spec.transform.position.z - sourceBody.spec.transform.position.z};
    const float distance = std::sqrt(direction.x * direction.x + direction.y * direction.y
                                     + direction.z * direction.z);
    if (!std::isfinite(distance) || distance <= 0.0F) {
        return false;
    }
    backend::RayQuery query{};
    query.scene = slot.physicsScene;
    query.stableOwnerId = source.key.id;
    query.shapeId = sourceBody.spec.shapeId;
    query.gate = sourceBody.spec.gate;
    query.origin = sourceBody.spec.transform.position;
    query.direction = direction;
    query.maxDistance = distance;
    query.ignoreOwnerId = source.key.id;
    backend::QueryHitBuffer hits{};
    return slot.physics.raycast(query, hits) == backend::QueryStatus::success && hits.count != 0
           && hits.hits[0].body == targetBinding->body;
}

/** @return Canonical world event for one combat mechanics outcome. */
[[nodiscard]] world::CommittedEvent combat_event(const mechanics::CombatEvent& source,
                                                 const world::ActorSnapshot& target) noexcept {
    world::CommittedEvent event{};
    event.actor = target.key;
    event.transform = target.transform;
    event.commandId = source.commandId;
    event.tick = source.tick;
    event.actorRevision = source.targetRevision;
    event.authorityGeneration = target.authority.generation;
    event.value = source.kind == mechanics::CombatEventKind::damageRejected
                      ? static_cast<std::uint64_t>(source.rejectReason)
                      : source.attackBlueprintId;
    event.kind = source.kind == mechanics::CombatEventKind::damageRejected
                     ? world::CommittedEventKind::serviceTickCommitted
                     : world::CommittedEventKind::damageCommitted;
    return event;
}

} // namespace

/** Resolves one checked damage intent before its world command is remembered. */
bool execute_damage_command(BubbleHost::Storage::WorldSlot& slot,
                            const world::HostExecutionContext& context,
                            const world::HostCommand& command,
                            std::span<const world::ActorSnapshot> actors,
                            EventWriter& writer) noexcept {
    if (world::command_kind(command) != world::HostCommandKind::submitDamage
        || !record_combat_poses(slot, context, actors)) {
        return false;
    }
    const auto& damage = std::get<world::SubmitDamageCommand>(command.payload);
    BubbleHost::Storage::CombatantBinding* sourceBinding = find_combatant(slot, damage.sourceActor);
    BubbleHost::Storage::CombatantBinding* targetBinding = find_combatant(slot, damage.targetActor);
    BubbleHost::Storage::AttackProfileSlot* attack = find_attack(slot, damage.attack);
    const world::ActorSnapshot* source = find_actor(actors, damage.sourceActor);
    const world::ActorSnapshot* target = find_actor(actors, damage.targetActor);
    if (sourceBinding == nullptr || targetBinding == nullptr || attack == nullptr
        || source == nullptr || target == nullptr) {
        return false;
    }
    mechanics::DamageIntent intent{};
    intent.command = mechanics_header(context, command);
    intent.sourceActor = {damage.sourceActor.id, damage.sourceActor.generation};
    intent.targetActor = {damage.targetActor.id, damage.targetActor.generation};
    intent.attackBlueprint = attack->attack;
    intent.claimedTick = damage.claimedTick;
    intent.hitEvidence.sourceAuthorityEpoch = source->authority.generation;
    intent.hitEvidence.lineOfSight = line_of_sight(slot, *source, *target);
    std::array<mechanics::CombatEvent, 3> combatEvents{};
    std::size_t eventCount = 0;
    const mechanics::CombatResult result =
        slot.combat.submit_damage(intent, combatEvents, eventCount);
    if (result != mechanics::CombatResult::applied && result != mechanics::CombatResult::rejected) {
        return false;
    }
    for (std::size_t index = 0; index < eventCount; ++index) {
        if (!writer.append(combat_event(combatEvents[index], *target))) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::host::detail
