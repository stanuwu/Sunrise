#pragma once

#include "actor_store.h"

namespace sunrise::server::gameplay::physics::world {

/** Exact result of one authority check or transition. */
enum class AuthorityStatus : std::uint8_t {
    applied,
    invalid,
    notFound,
    wrongOwner,
    staleActor,
    staleAuthority,
    capacity,
};

/** Validates authority epochs before ActorStore state changes. */
class AuthorityManager final {
public:
    /** @return Applied only for a strict next epoch valid on the following tick. */
    [[nodiscard]] AuthorityStatus transfer(ActorStore& actors,
                                           const HostCommandHeader& header,
                                           const SetMotionAuthorityCommand& command,
                                           TickId commitTick,
                                           ActorSnapshot& committed) const noexcept;
    /** @return Applied when source, owner, actor, and authority all match. */
    [[nodiscard]] AuthorityStatus validate_motion(const ActorStore& actors,
                                                  const HostCommandHeader& header,
                                                  const ActorKey& actor,
                                                  std::uint64_t authorityGeneration,
                                                  TickId currentTick,
                                                  bool adminTeleport) const noexcept;
    /** @return Applied when one service pose has exact active authority. */
    [[nodiscard]] AuthorityStatus validate_pose_commit(const ActorStore& actors,
                                                       const CanonicalPoseCommit& commit,
                                                       TickId currentTick) const noexcept;
};

} // namespace sunrise::server::gameplay::physics::world
