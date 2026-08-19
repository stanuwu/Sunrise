#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

/** @return The policy-controlled actor role, or the absent actor. */
[[nodiscard]] ActorKey controlled_actor(const HostCommand& command) noexcept {
    switch (command_kind(command)) {
    case HostCommandKind::spawnActor:
        return std::get<SpawnActorCommand>(command.payload).parent;
    case HostCommandKind::removeActor:
        return std::get<RemoveActorCommand>(command.payload).actor;
    case HostCommandKind::setMotionAuthority:
        return std::get<SetMotionAuthorityCommand>(command.payload).actor;
    case HostCommandKind::setKinematicTarget:
        return std::get<SetKinematicTargetCommand>(command.payload).actor;
    case HostCommandKind::teleportActor:
        return std::get<TeleportActorCommand>(command.payload).actor;
    case HostCommandKind::configureController:
        return std::get<ConfigureControllerCommand>(command.payload).actor;
    case HostCommandKind::requestPath:
        return std::get<RequestPathCommand>(command.payload).actor;
    case HostCommandKind::configureCombat:
        return std::get<ConfigureCombatCommand>(command.payload).actor;
    case HostCommandKind::submitDamage:
        return std::get<SubmitDamageCommand>(command.payload).sourceActor;
    case HostCommandKind::emitMappedIncident:
        return std::get<EmitMappedIncidentCommand>(command.payload).sourceActor;
    case HostCommandKind::createTrigger:
    case HostCommandKind::removeTrigger:
    case HostCommandKind::addObjectiveCounter:
    case HostCommandKind::setObjectiveCounter:
    case HostCommandKind::recordParticipantCredit:
    case HostCommandKind::scheduleTickTimer:
    case HostCommandKind::requestCheckpoint:
    case HostCommandKind::submitRewardIntent:
    case HostCommandKind::count:
        return {};
    }
    return {};
}

} // namespace

/** Checks the policy-owned actor role while leaving target actors unrestricted. */
bool WorldRunner::policy_actor_access(const HostCommand& command) const noexcept {
    if (command.header.source.kind != CommandSourceKind::policy) {
        return true;
    }
    const ActorKey actor = controlled_actor(command);
    if (!valid_actor(actor)) {
        return true;
    }
    ActorSnapshot current{};
    return actors_.find(actor, current) && current.policyOwnerId == command.header.policyOwnerId;
}

} // namespace sunrise::server::gameplay::physics::world
