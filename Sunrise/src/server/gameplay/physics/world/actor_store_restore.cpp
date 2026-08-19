#include <algorithm>
#include <bit>

#include "actor_store.h"

namespace sunrise::server::gameplay::physics::world {
namespace {

[[nodiscard]] bool absent_actor(const ActorKey& actor) noexcept {
    return actor.id == 0 && actor.generation == 0;
}

[[nodiscard]] bool valid_authority(const MotionAuthority& authority) noexcept {
    if (authority.generation == 0 || authority.validFromTick == 0
        || static_cast<std::uint8_t>(authority.mode)
               >= static_cast<std::uint8_t>(MotionAuthorityMode::count)) {
        return false;
    }
    return authority.mode == MotionAuthorityMode::host ? authority.ownerMemberId == 0
                                                       : authority.ownerMemberId != 0;
}

[[nodiscard]] const ActorSnapshot* find_actor(std::span<const ActorSnapshot> actors,
                                              const ActorKey& key) noexcept {
    for (const ActorSnapshot& actor : actors) {
        if (equal_actor(actor.key, key)) {
            return &actor;
        }
    }
    return nullptr;
}

[[nodiscard]] bool same_float(float left, float right) noexcept {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

/** @return True when a transform already uses canonical float bits. */
[[nodiscard]] bool canonical_transform(const Transform& transform) noexcept {
    Transform canonical = transform;
    return canonicalize_transform(canonical)
           && same_float(canonical.position.x, transform.position.x)
           && same_float(canonical.position.y, transform.position.y)
           && same_float(canonical.position.z, transform.position.z)
           && same_float(canonical.rotation.x, transform.rotation.x)
           && same_float(canonical.rotation.y, transform.rotation.y)
           && same_float(canonical.rotation.z, transform.rotation.z)
           && same_float(canonical.rotation.w, transform.rotation.w);
}

/** @return True when every parent chain is closed and acyclic. */
[[nodiscard]] bool valid_parent_chains(std::span<const ActorSnapshot> actors) noexcept {
    for (const ActorSnapshot& actor : actors) {
        ActorKey parent = actor.parent;
        for (std::size_t depth = 0; !absent_actor(parent); ++depth) {
            if (depth >= actors.size() || equal_actor(parent, actor.key)) {
                return false;
            }
            const ActorSnapshot* found = find_actor(actors, parent);
            if (found == nullptr) {
                return false;
            }
            parent = found->parent;
        }
    }
    return true;
}

/** @return True when actors form one canonical restorable span. */
[[nodiscard]] bool valid_actors(std::span<const ActorSnapshot> actors) noexcept {
    if (actors.size() > kActorCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < actors.size(); ++index) {
        const ActorSnapshot& actor = actors[index];
        if (!valid_actor(actor.key) || actor.policyOwnerId == 0 || !valid_blueprint(actor.blueprint)
            || !canonical_transform(actor.transform) || actor.revision == 0
            || actor.lifecycle != ActorLifecycle::live
            || (!absent_actor(actor.parent) && !valid_actor(actor.parent))
            || !valid_authority(actor.authority)) {
            return false;
        }
        if (index != 0 && actors[index - 1].key.id >= actor.key.id) {
            return false;
        }
    }
    return valid_parent_chains(actors);
}

/** @return True when retired high-water entries are sorted and exact. */
[[nodiscard]] bool valid_retired(std::span<const ActorSnapshot> actors,
                                 std::span<const RetiredActorGeneration> retired) noexcept {
    if (retired.size() > kActorGenerationHistoryCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < retired.size(); ++index) {
        const RetiredActorGeneration& entry = retired[index];
        if (entry.actorId == 0 || entry.generation == 0 || entry.retiredTick == 0
            || (index != 0 && retired[index - 1].actorId >= entry.actorId)) {
            return false;
        }
        for (const ActorSnapshot& actor : actors) {
            if (actor.key.id == entry.actorId && actor.key.generation <= entry.generation) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/** @return True when actor and retired-generation spans are canonical and restorable. */
bool ActorStore::valid_restore(std::span<const ActorSnapshot> actors,
                               std::span<const RetiredActorGeneration> retired) noexcept {
    return valid_actors(actors) && valid_retired(actors, retired);
}

/** Restores actors and high-water entries after complete validation. */
bool ActorStore::restore(std::span<const ActorSnapshot> actors,
                         std::span<const RetiredActorGeneration> retired) noexcept {
    if (!valid_restore(actors, retired)) {
        return false;
    }
    slots_ = {};
    tombstones_ = {};
    size_ = actors.size();
    tombstoneCount_ = retired.size();
    for (std::size_t index = 0; index < actors.size(); ++index) {
        slots_[index].actor = actors[index];
        slots_[index].occupied = true;
    }
    std::copy(retired.begin(), retired.end(), tombstones_.begin());
    return true;
}

} // namespace sunrise::server::gameplay::physics::world
