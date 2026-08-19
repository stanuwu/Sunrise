#include <limits>

#include "actor_store.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

/** @return True when no earlier commit names the same exact actor. */
[[nodiscard]] bool unique_actor(std::span<const CanonicalPoseCommit> commits,
                                std::size_t index) noexcept {
    for (std::size_t prior = 0; prior < index; ++prior) {
        if (equal_actor(commits[prior].actor, commits[index].actor)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Commits a preflighted pose batch without partial actor mutation. */
bool ActorStore::commit_pose_batch(std::span<const CanonicalPoseCommit> commits,
                                   std::span<ActorSnapshot> committed) noexcept {
    if (commits.size() > kActorCapacity || committed.size() < commits.size()) {
        return false;
    }
    std::array<Slot*, kActorCapacity> targets{};
    std::array<Transform, kActorCapacity> transforms{};
    for (std::size_t index = 0; index < commits.size(); ++index) {
        const CanonicalPoseCommit& commit = commits[index];
        Slot* slot = find_slot(commit.actor);
        transforms[index] = commit.transform;
        if (slot == nullptr || !canonicalize_transform(transforms[index])
            || !unique_actor(commits, index)
            || slot->actor.revision == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        targets[index] = slot;
    }
    for (std::size_t index = 0; index < commits.size(); ++index) {
        Slot& slot = *targets[index];
        slot.actor.transform = transforms[index];
        ++slot.actor.revision;
        committed[index] = slot.actor;
    }
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
