#include "authority_manager.h"

#include <limits>

namespace sunrise::server::gameplay::physics::world {
namespace {

[[nodiscard]] AuthorityStatus missing_status(const ActorStore& actors,
                                             const ActorKey& actor) noexcept {
    ActorSnapshot sameId{};
    return actors.find_by_id(actor.id, sameId) ? AuthorityStatus::staleActor
                                               : AuthorityStatus::notFound;
}

[[nodiscard]] bool valid_source(const CommandSource& source) noexcept {
    return source.id != 0
           && static_cast<std::uint8_t>(source.kind)
                  < static_cast<std::uint8_t>(CommandSourceKind::count);
}

/** @return True when this exact source may drive mechanics under the current mode. */
[[nodiscard]] bool source_can_move(const MotionAuthority& authority,
                                   const CommandSource& source,
                                   bool adminTeleport) noexcept {
    if (adminTeleport) {
        return source.kind == CommandSourceKind::host || source.kind == CommandSourceKind::policy;
    }
    if (source.kind == CommandSourceKind::host || source.kind == CommandSourceKind::policy) {
        return authority.mode == MotionAuthorityMode::host && authority.ownerMemberId == 0;
    }
    return source.kind == CommandSourceKind::network
           && authority.mode == MotionAuthorityMode::clientPredictedHostChecked
           && authority.ownerMemberId == source.id;
}

[[nodiscard]] bool policy_owner_matches(const ActorSnapshot& actor,
                                        const HostCommandHeader& header) noexcept {
    return header.source.kind != CommandSourceKind::policy
           || actor.policyOwnerId == header.policyOwnerId;
}

} // namespace

/** Commits one strictly increasing authority epoch. */
AuthorityStatus AuthorityManager::transfer(ActorStore& actors,
                                           const HostCommandHeader& header,
                                           const SetMotionAuthorityCommand& command,
                                           TickId commitTick,
                                           ActorSnapshot& committed) const noexcept {
    committed = {};
    if (!valid_actor(command.actor) || commitTick == std::numeric_limits<TickId>::max()
        || command.expectedAuthorityGeneration == 0
        || command.expectedAuthorityGeneration == std::numeric_limits<std::uint64_t>::max()
        || command.nextAuthorityGeneration != command.expectedAuthorityGeneration + 1U
        || command.validFromTick != commitTick + 1U
        || (command.mode == MotionAuthorityMode::host && command.ownerMemberId != 0)
        || ((command.mode == MotionAuthorityMode::clientPredictedHostChecked
             || command.mode == MotionAuthorityMode::clientPresentational)
            && command.ownerMemberId == 0)
        || static_cast<std::uint8_t>(command.mode)
               >= static_cast<std::uint8_t>(MotionAuthorityMode::count)) {
        return AuthorityStatus::invalid;
    }

    ActorSnapshot current{};
    if (!actors.find(command.actor, current)) {
        return missing_status(actors, command.actor);
    }
    if (!policy_owner_matches(current, header)) {
        return AuthorityStatus::wrongOwner;
    }
    if (current.authority.generation != command.expectedAuthorityGeneration) {
        return AuthorityStatus::staleAuthority;
    }

    MotionAuthority next{};
    next.ownerMemberId = command.ownerMemberId;
    next.generation = command.nextAuthorityGeneration;
    next.validFromTick = command.validFromTick;
    next.mode = command.mode;
    const ActorStoreStatus status = actors.set_authority(command.actor, next, committed);
    if (status == ActorStoreStatus::applied) {
        return AuthorityStatus::applied;
    }
    return status == ActorStoreStatus::capacity ? AuthorityStatus::capacity
                                                : AuthorityStatus::invalid;
}

/** Validates source, owner, actor, and authority before motion mutation. */
AuthorityStatus AuthorityManager::validate_motion(const ActorStore& actors,
                                                  const HostCommandHeader& header,
                                                  const ActorKey& actor,
                                                  std::uint64_t authorityGeneration,
                                                  TickId currentTick,
                                                  bool adminTeleport) const noexcept {
    if (!valid_actor(actor) || authorityGeneration == 0 || !valid_source(header.source)) {
        return AuthorityStatus::invalid;
    }
    ActorSnapshot current{};
    if (!actors.find(actor, current)) {
        return missing_status(actors, actor);
    }
    if (!policy_owner_matches(current, header)
        || !source_can_move(current.authority, header.source, adminTeleport)) {
        return AuthorityStatus::wrongOwner;
    }
    return current.authority.generation != authorityGeneration
                   || currentTick < current.authority.validFromTick
               ? AuthorityStatus::staleAuthority
               : AuthorityStatus::applied;
}

/** Validates one commandless service pose against exact active authority. */
AuthorityStatus AuthorityManager::validate_pose_commit(const ActorStore& actors,
                                                       const CanonicalPoseCommit& commit,
                                                       TickId currentTick) const noexcept {
    if (!valid_actor(commit.actor) || !finite_transform(commit.transform)
        || commit.policyOwnerId == 0 || commit.authorityGeneration == 0
        || !valid_source(commit.source)
        || (commit.source.kind != CommandSourceKind::host
            && commit.source.kind != CommandSourceKind::network)) {
        return AuthorityStatus::invalid;
    }
    ActorSnapshot current{};
    if (!actors.find(commit.actor, current)) {
        return missing_status(actors, commit.actor);
    }
    if (current.policyOwnerId != commit.policyOwnerId
        || !source_can_move(current.authority, commit.source, false)) {
        return AuthorityStatus::wrongOwner;
    }
    return current.authority.generation != commit.authorityGeneration
                   || currentTick < current.authority.validFromTick
               ? AuthorityStatus::staleAuthority
               : AuthorityStatus::applied;
}

} // namespace sunrise::server::gameplay::physics::world
