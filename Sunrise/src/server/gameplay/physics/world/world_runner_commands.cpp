#include "world_runner.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

/** @return Runner rejection reason for one ActorStore result. */
[[nodiscard]] CommandRejectReason actor_reject_reason(ActorStoreStatus status) noexcept {
    switch (status) {
    case ActorStoreStatus::staleActor:
        return CommandRejectReason::staleActor;
    case ActorStoreStatus::capacity:
    case ActorStoreStatus::generationHistoryFull:
        return CommandRejectReason::capacity;
    case ActorStoreStatus::duplicate:
        return CommandRejectReason::duplicate;
    case ActorStoreStatus::invalid:
    case ActorStoreStatus::notFound:
    case ActorStoreStatus::applied:
        return CommandRejectReason::invalid;
    }
    return CommandRejectReason::invalid;
}

/** @return Runner rejection reason for one AuthorityManager result. */
[[nodiscard]] CommandRejectReason authority_reject_reason(AuthorityStatus status) noexcept {
    switch (status) {
    case AuthorityStatus::staleActor:
        return CommandRejectReason::staleActor;
    case AuthorityStatus::wrongOwner:
        return CommandRejectReason::wrongOwner;
    case AuthorityStatus::staleAuthority:
        return CommandRejectReason::staleAuthority;
    case AuthorityStatus::capacity:
        return CommandRejectReason::capacity;
    case AuthorityStatus::invalid:
    case AuthorityStatus::notFound:
    case AuthorityStatus::applied:
        return CommandRejectReason::invalid;
    }
    return CommandRejectReason::invalid;
}

} // namespace

/**
 * Applies one ordered command or records one bounded rejection.
 * @param command Command selected for the current exact tick.
 * @param commandOrdinal Stable position in this tick's ordered command batch.
 */
void WorldRunner::apply_command(const HostCommand& command, std::size_t commandOrdinal) noexcept {
    if (!equal_world(command.header.world, identity_)) {
        reject(command.header.world.ownerGeneration == identity_.ownerGeneration
                   ? CommandRejectReason::wrongWorld
                   : CommandRejectReason::wrongOwner);
        return;
    }
    if (command.header.executeTick != tick_) {
        reject(CommandRejectReason::late);
        return;
    }
    if (seen_command(command.header)) {
        reject(CommandRejectReason::duplicate);
        return;
    }
    const HostCommandKind kind = command_kind(command);
    if ((command.header.source.kind == CommandSourceKind::network
         && kind != HostCommandKind::setKinematicTarget)
        || command.header.source.kind == CommandSourceKind::timer
        || command.header.source.kind == CommandSourceKind::count) {
        reject(CommandRejectReason::policyDenied);
        return;
    }
    if (!policy_actor_access(command)) {
        reject(CommandRejectReason::wrongOwner);
        return;
    }
    if (committedEventCount_ == committedEvents_.size()) {
        healthy_ = false;
        reject(CommandRejectReason::capacity);
        return;
    }
    if (commandHistoryCount_ == commandHistory_.size()) {
        reject(CommandRejectReason::capacity);
        return;
    }
    if (command.header.source.kind == CommandSourceKind::host && !allowHostActorCommands_
        && command_uses_actor(kind)) {
        reject(CommandRejectReason::policyDenied);
        return;
    }

    ActorSnapshot actor{};
    switch (kind) {
    case HostCommandKind::spawnActor: {
        if (command.header.source.kind == CommandSourceKind::policy && manifest_.actorBudget == 0) {
            reject(CommandRejectReason::policyDenied);
            return;
        }
        if (command.header.source.kind == CommandSourceKind::policy
            && actors_.size() >= manifest_.actorBudget) {
            reject(CommandRejectReason::capacity);
            return;
        }
        const ActorStoreStatus status = actors_.spawn(
            command.header, std::get<SpawnActorCommand>(command.payload), tick_, actor);
        if (status != ActorStoreStatus::applied) {
            reject(actor_reject_reason(status));
            return;
        }
        publish_event(command, CommittedEventKind::actorSpawned, actor);
        break;
    }
    case HostCommandKind::removeActor: {
        const ActorKey key = std::get<RemoveActorCommand>(command.payload).actor;
        const ActorStoreStatus status = actors_.remove(key, tick_, actor);
        if (status == ActorStoreStatus::generationHistoryFull) {
            healthy_ = false;
        }
        if (status != ActorStoreStatus::applied) {
            reject(actor_reject_reason(status));
            return;
        }
        publish_event(command, CommittedEventKind::actorRemoved, actor);
        break;
    }
    case HostCommandKind::setMotionAuthority: {
        const AuthorityStatus status =
            authority_.transfer(actors_,
                                command.header,
                                std::get<SetMotionAuthorityCommand>(command.payload),
                                tick_,
                                actor);
        if (status != AuthorityStatus::applied) {
            reject(authority_reject_reason(status));
            return;
        }
        publish_event(command, CommittedEventKind::authorityChanged, actor);
        break;
    }
    case HostCommandKind::setKinematicTarget:
    case HostCommandKind::teleportActor: {
        ActorKey key{};
        Transform target{};
        std::uint64_t authorityGeneration = 0;
        if (kind == HostCommandKind::setKinematicTarget) {
            const auto& motion = std::get<SetKinematicTargetCommand>(command.payload);
            key = motion.actor;
            target = motion.target;
            authorityGeneration = motion.authorityGeneration;
        } else {
            const auto& teleport = std::get<TeleportActorCommand>(command.payload);
            key = teleport.actor;
            target = teleport.target;
            authorityGeneration = teleport.authorityGeneration;
        }
        const AuthorityStatus authorityStatus =
            authority_.validate_motion(actors_,
                                       command.header,
                                       key,
                                       authorityGeneration,
                                       tick_,
                                       kind == HostCommandKind::teleportActor);
        if (authorityStatus != AuthorityStatus::applied) {
            reject(authority_reject_reason(authorityStatus));
            return;
        }
        const ActorStoreStatus actorStatus = actors_.set_transform(key, target, actor);
        if (actorStatus != ActorStoreStatus::applied) {
            reject(actor_reject_reason(actorStatus));
            return;
        }
        publish_event(command,
                      kind == HostCommandKind::setKinematicTarget
                          ? CommittedEventKind::kinematicTargetChanged
                          : CommittedEventKind::actorTeleported,
                      actor);
        break;
    }
    case HostCommandKind::requestCheckpoint: {
        const auto& checkpoint = std::get<RequestCheckpointCommand>(command.payload);
        if (checkpoint.reasonId == 0) {
            reject(CommandRejectReason::invalid);
            return;
        }
        publish_event(command, CommittedEventKind::checkpointRequested, actor, checkpoint.reasonId);
        break;
    }
    case HostCommandKind::submitRewardIntent: {
        const auto& reward = std::get<SubmitRewardIntentCommand>(command.payload);
        if (reward.participantId == 0 || reward.rewardIntentId == 0
            || !valid_blueprint(reward.rewardTable)) {
            reject(CommandRejectReason::invalid);
            return;
        }
        publish_event(
            command, CommittedEventKind::rewardIntentSubmitted, actor, reward.rewardIntentId);
        break;
    }
    case HostCommandKind::configureController:
    case HostCommandKind::requestPath:
    case HostCommandKind::configureCombat:
    case HostCommandKind::submitDamage:
    case HostCommandKind::createTrigger:
    case HostCommandKind::removeTrigger:
    case HostCommandKind::addObjectiveCounter:
    case HostCommandKind::setObjectiveCounter:
    case HostCommandKind::recordParticipantCredit:
    case HostCommandKind::emitMappedIncident:
    case HostCommandKind::scheduleTickTimer: {
        if (!execute_extension_command(command, commandOrdinal)) {
            return;
        }
        remember_command(command.header);
        ++metrics_.commandsApplied;
        tickCommandApplied_[commandOrdinal] = true;
        tickCommandExtension_[commandOrdinal] = true;
        return;
    }
    case HostCommandKind::count:
        reject(CommandRejectReason::invalid);
        return;
    }
    remember_command(command.header);
    ++metrics_.commandsApplied;
    tickCommandApplied_[commandOrdinal] = true;
}

} // namespace sunrise::server::gameplay::physics::world
