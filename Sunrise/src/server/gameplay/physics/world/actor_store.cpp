#include "actor_store.h"

#include <algorithm>
#include <limits>

namespace sunrise::server::gameplay::physics::world {
namespace {

/** A committed spawn begins at actor revision one. */
constexpr std::uint64_t kInitialActorRevision = 1;

[[nodiscard]] bool absent_actor(const ActorKey& actor) noexcept {
    return actor.id == 0 && actor.generation == 0;
}

[[nodiscard]] bool valid_authority_owner(MotionAuthorityMode mode,
                                         std::uint64_t ownerMemberId) noexcept {
    if (mode == MotionAuthorityMode::host) {
        return ownerMemberId == 0;
    }
    return (mode == MotionAuthorityMode::clientPredictedHostChecked
            || mode == MotionAuthorityMode::clientPresentational)
           && ownerMemberId != 0;
}

} // namespace

/** Clears all live actors and retired-generation guards. */
void ActorStore::reset() noexcept {
    slots_ = {};
    tombstones_ = {};
    size_ = 0;
    tombstoneCount_ = 0;
}

/**
 * Commits one exact logical actor into fixed storage.
 * @param header Policy owner and versioned blueprint.
 * @param command Complete spawn state and actor generation.
 * @param tick Commit tick used by initial authority.
 * @param committed Receives the pointer-free committed state.
 * @return Applied or one exact rejection class.
 */
ActorStoreStatus ActorStore::spawn(const HostCommandHeader& header,
                                   const SpawnActorCommand& command,
                                   TickId tick,
                                   ActorSnapshot& committed) noexcept {
    committed = {};
    Transform canonicalTransform = command.transform;
    if (!valid_actor(command.actor) || !valid_blueprint(header.definition)
        || !canonicalize_transform(canonicalTransform) || command.initialAuthorityGeneration == 0
        || !valid_authority_owner(command.authorityMode, command.authorityOwnerMemberId)
        || (!absent_actor(command.parent) && !valid_actor(command.parent))) {
        return ActorStoreStatus::invalid;
    }
    if (!absent_actor(command.parent) && find_slot(command.parent) == nullptr) {
        return find_id_slot(command.parent.id) == nullptr ? ActorStoreStatus::notFound
                                                          : ActorStoreStatus::staleActor;
    }

    if (const Slot* existing = find_id_slot(command.actor.id); existing != nullptr) {
        return equal_actor(existing->actor.key, command.actor) ? ActorStoreStatus::duplicate
                                                               : ActorStoreStatus::staleActor;
    }
    if (command.actor.generation <= last_generation(command.actor.id)) {
        return ActorStoreStatus::staleActor;
    }
    if (size_ == slots_.size()) {
        return ActorStoreStatus::capacity;
    }

    for (Slot& slot : slots_) {
        if (slot.occupied) {
            continue;
        }
        slot.actor.key = command.actor;
        slot.actor.policyOwnerId = header.policyOwnerId;
        slot.actor.blueprint = header.definition;
        slot.actor.parent = command.parent;
        slot.actor.transform = canonicalTransform;
        slot.actor.authority.ownerMemberId = command.authorityOwnerMemberId;
        slot.actor.authority.generation = command.initialAuthorityGeneration;
        slot.actor.authority.validFromTick = tick;
        slot.actor.authority.mode = command.authorityMode;
        slot.actor.revision = kInitialActorRevision;
        slot.actor.bubble = command.bubble;
        slot.actor.lifecycle = ActorLifecycle::live;
        slot.occupied = true;
        ++size_;
        committed = slot.actor;
        return ActorStoreStatus::applied;
    }
    return ActorStoreStatus::capacity;
}

/**
 * Removes one exact actor after all live children are gone.
 * @param actor Exact logical actor generation.
 * @param removed Receives the final committed state.
 * @return Applied or one exact rejection class.
 */
ActorStoreStatus
ActorStore::remove(const ActorKey& actor, TickId retiredTick, ActorSnapshot& removed) noexcept {
    removed = {};
    if (!valid_actor(actor) || retiredTick == 0) {
        return ActorStoreStatus::invalid;
    }
    Slot* slot = find_slot(actor);
    if (slot == nullptr) {
        return find_id_slot(actor.id) == nullptr ? ActorStoreStatus::notFound
                                                 : ActorStoreStatus::staleActor;
    }

    for (const Slot& child : slots_) {
        if (child.occupied && equal_actor(child.actor.parent, actor)) {
            return ActorStoreStatus::invalid;
        }
    }
    if (slot->actor.revision == std::numeric_limits<std::uint64_t>::max()) {
        return ActorStoreStatus::capacity;
    }
    if (!can_remember_generation(actor.id)) {
        return ActorStoreStatus::generationHistoryFull;
    }

    removed = slot->actor;
    ++removed.revision;
    remember_generation(actor, retiredTick);
    *slot = {};
    --size_;
    return ActorStoreStatus::applied;
}

/**
 * Replaces one exact actor transform and advances its revision.
 * @param actor Exact logical actor generation.
 * @param transform Finite canonical transform.
 * @param committed Receives the new state.
 * @return Applied or one exact rejection class.
 */
ActorStoreStatus ActorStore::set_transform(const ActorKey& actor,
                                           const Transform& transform,
                                           ActorSnapshot& committed) noexcept {
    committed = {};
    Transform canonicalTransform = transform;
    if (!valid_actor(actor) || !canonicalize_transform(canonicalTransform)) {
        return ActorStoreStatus::invalid;
    }
    Slot* slot = find_slot(actor);
    if (slot == nullptr) {
        return find_id_slot(actor.id) == nullptr ? ActorStoreStatus::notFound
                                                 : ActorStoreStatus::staleActor;
    }
    if (slot->actor.revision == std::numeric_limits<std::uint64_t>::max()) {
        return ActorStoreStatus::capacity;
    }
    slot->actor.transform = canonicalTransform;
    ++slot->actor.revision;
    committed = slot->actor;
    return ActorStoreStatus::applied;
}

/**
 * Replaces authority after AuthorityManager validates the transition.
 * @param actor Exact logical actor generation.
 * @param authority Complete next authority state.
 * @param committed Receives the new state.
 * @return Applied or one exact rejection class.
 */
ActorStoreStatus ActorStore::set_authority(const ActorKey& actor,
                                           const MotionAuthority& authority,
                                           ActorSnapshot& committed) noexcept {
    committed = {};
    if (!valid_actor(actor) || authority.generation == 0
        || !valid_authority_owner(authority.mode, authority.ownerMemberId)) {
        return ActorStoreStatus::invalid;
    }
    Slot* slot = find_slot(actor);
    if (slot == nullptr) {
        return find_id_slot(actor.id) == nullptr ? ActorStoreStatus::notFound
                                                 : ActorStoreStatus::staleActor;
    }
    if (slot->actor.revision == std::numeric_limits<std::uint64_t>::max()) {
        return ActorStoreStatus::capacity;
    }
    slot->actor.authority = authority;
    ++slot->actor.revision;
    committed = slot->actor;
    return ActorStoreStatus::applied;
}

bool ActorStore::find(const ActorKey& actor, ActorSnapshot& snapshot) const noexcept {
    snapshot = {};
    const Slot* slot = find_slot(actor);
    if (slot == nullptr) {
        return false;
    }
    snapshot = slot->actor;
    return true;
}

bool ActorStore::find_by_id(ActorId actorId, ActorSnapshot& snapshot) const noexcept {
    snapshot = {};
    const Slot* slot = find_id_slot(actorId);
    if (slot == nullptr) {
        return false;
    }
    snapshot = slot->actor;
    return true;
}

/**
 * Copies actors in stable logical identity order.
 * @param output Fixed caller storage.
 * @return Number of actors written.
 */
std::size_t ActorStore::copy_sorted(std::span<ActorSnapshot> output) const noexcept {
    std::size_t written = 0;
    for (const Slot& slot : slots_) {
        if (slot.occupied && written < output.size()) {
            output[written++] = slot.actor;
        }
    }
    std::sort(output.begin(),
              output.begin() + static_cast<std::ptrdiff_t>(written),
              [](const ActorSnapshot& first, const ActorSnapshot& second) noexcept {
                  return first.key.id < second.key.id
                         || (first.key.id == second.key.id
                             && first.key.generation < second.key.generation);
              });
    return written;
}

std::size_t ActorStore::size() const noexcept {
    return size_;
}

ActorStore::Slot* ActorStore::find_slot(const ActorKey& actor) noexcept {
    for (Slot& slot : slots_) {
        if (slot.occupied && equal_actor(slot.actor.key, actor)) {
            return &slot;
        }
    }
    return nullptr;
}

const ActorStore::Slot* ActorStore::find_slot(const ActorKey& actor) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.occupied && equal_actor(slot.actor.key, actor)) {
            return &slot;
        }
    }
    return nullptr;
}

ActorStore::Slot* ActorStore::find_id_slot(ActorId actorId) noexcept {
    for (Slot& slot : slots_) {
        if (slot.occupied && slot.actor.key.id == actorId) {
            return &slot;
        }
    }
    return nullptr;
}

const ActorStore::Slot* ActorStore::find_id_slot(ActorId actorId) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.occupied && slot.actor.key.id == actorId) {
            return &slot;
        }
    }
    return nullptr;
}

} // namespace sunrise::server::gameplay::physics::world
