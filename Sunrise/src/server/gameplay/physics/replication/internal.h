#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "definition.h"

namespace sunrise::server::gameplay::physics::replication::detail {

[[nodiscard]] inline bool
equal_context(const state::gameplay::physics::ContextHandle& left,
              const state::gameplay::physics::ContextHandle& right) noexcept {
    return left.slot == right.slot && left.generation == right.generation;
}

[[nodiscard]] inline bool
equal_allocation(const state::gameplay::physics::Allocation& left,
                 const state::gameplay::physics::Allocation& right) noexcept {
    return equal_context(left.context, right.context) && left.token.index == right.token.index
           && left.token.incarnation == right.token.incarnation && left.actorId == right.actorId
           && left.generation == right.generation && left.sequence == right.sequence;
}

/** @return True when every transform component is finite. */
[[nodiscard]] inline bool finite_transform(const Transform& transform) noexcept {
    for (const float value : transform.orientation) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    for (const float value : transform.position) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool valid_snapshot(const ActorSnapshot& actor) noexcept {
    return actor.allocation.actorId != state::gameplay::physics::kAbsentActorId
           && actor.allocation.context.slot != state::gameplay::physics::kInvalidContextSlot
           && actor.allocation.context.generation
                  != state::gameplay::physics::kAbsentHandleGeneration
           && actor.allocation.generation != state::gameplay::physics::kAbsentHandleGeneration
           && actor.versions.lifecycle != 0 && finite_transform(actor.transform);
}

[[nodiscard]] inline bool monotonic(const FieldVersions& current,
                                    const FieldVersions& candidate) noexcept {
    return candidate.lifecycle >= current.lifecycle && candidate.authority >= current.authority
           && candidate.parent >= current.parent && candidate.combat >= current.combat
           && candidate.content >= current.content && candidate.transform >= current.transform;
}

/** @return True when unchanged field versions retain their exact payload. */
[[nodiscard]] inline bool consistent_update(const ActorSnapshot& current,
                                            const ActorSnapshot& candidate) noexcept {
    if (candidate.versions.authority == current.versions.authority
        && candidate.authorityEpoch != current.authorityEpoch) {
        return false;
    }
    if (candidate.versions.parent == current.versions.parent
        && (candidate.parentActorId != current.parentActorId
            || candidate.streamSourceActorId != current.streamSourceActorId)) {
        return false;
    }
    if (candidate.versions.combat == current.versions.combat
        && candidate.combatRevision != current.combatRevision) {
        return false;
    }
    if (candidate.versions.content == current.versions.content
        && (candidate.contentRevision != current.contentRevision
            || candidate.bubble != current.bubble)) {
        return false;
    }
    return candidate.versions.transform != current.versions.transform
           || (candidate.transform.orientation == current.transform.orientation
               && candidate.transform.position == current.transform.position);
}

[[nodiscard]] inline bool equal_versions(const FieldVersions& left,
                                         const FieldVersions& right) noexcept {
    return left.lifecycle == right.lifecycle && left.authority == right.authority
           && left.parent == right.parent && left.combat == right.combat
           && left.content == right.content && left.transform == right.transform;
}

/** @return True when every selected payload still matches the desired actor. */
[[nodiscard]] inline bool equal_selected_payload(const ActorSnapshot& desired,
                                                 const ActorSnapshot& prepared,
                                                 FieldMask fields) noexcept {
    if ((fields & field_bit(Field::authority)) != 0
        && prepared.authorityEpoch != desired.authorityEpoch) {
        return false;
    }
    if ((fields & field_bit(Field::parent)) != 0
        && (prepared.parentActorId != desired.parentActorId
            || prepared.streamSourceActorId != desired.streamSourceActorId)) {
        return false;
    }
    if ((fields & field_bit(Field::combat)) != 0
        && prepared.combatRevision != desired.combatRevision) {
        return false;
    }
    if ((fields & field_bit(Field::content)) != 0
        && (prepared.contentRevision != desired.contentRevision
            || prepared.bubble != desired.bubble)) {
        return false;
    }
    return (fields & field_bit(Field::transform)) == 0
           || (prepared.transform.orientation == desired.transform.orientation
               && prepared.transform.position == desired.transform.position);
}

/** @return The selected field's monotonic version. */
[[nodiscard]] inline std::uint64_t field_version(const FieldVersions& versions,
                                                 Field field) noexcept {
    switch (field) {
    case Field::lifecycle:
        return versions.lifecycle;
    case Field::authority:
        return versions.authority;
    case Field::parent:
        return versions.parent;
    case Field::combat:
        return versions.combat;
    case Field::content:
        return versions.content;
    case Field::transform:
        return versions.transform;
    default:
        return 0;
    }
}

/** Replaces one selected field version. */
inline void set_field_version(FieldVersions& versions, Field field, std::uint64_t value) noexcept {
    switch (field) {
    case Field::lifecycle:
        versions.lifecycle = value;
        break;
    case Field::authority:
        versions.authority = value;
        break;
    case Field::parent:
        versions.parent = value;
        break;
    case Field::combat:
        versions.combat = value;
        break;
    case Field::content:
        versions.content = value;
        break;
    case Field::transform:
        versions.transform = value;
        break;
    default:
        break;
    }
}

[[nodiscard]] inline FieldMask dirty_fields(const ActorRecord& actor) noexcept {
    FieldMask result = 0;
    for (std::uint8_t value = 0; value < static_cast<std::uint8_t>(Field::count); ++value) {
        const auto field = static_cast<Field>(value);
        if (field_version(actor.desired.versions, field) > field_version(actor.baseline, field)) {
            result |= field_bit(field);
        }
    }
    return result;
}

/** @return Stable lifecycle priority for one pending operation. */
[[nodiscard]] inline std::uint8_t operation_priority(Operation operation) noexcept {
    switch (operation) {
    case Operation::remove:
        return 0;
    case Operation::create:
        return 1;
    case Operation::update:
        return 2;
    default:
        return 3;
    }
}

/** @return The next lifecycle operation allowed by one actor record. */
[[nodiscard]] inline Operation pending_operation(const ActorRecord& actor) noexcept {
    if (actor.contributionPending) {
        return Operation::none;
    }
    if (actor.removeRequested) {
        return actor.createCommitted ? Operation::remove : Operation::none;
    }
    if (!actor.createCommitted) {
        return Operation::create;
    }
    return dirty_fields(actor) != 0 ? Operation::update : Operation::none;
}

[[nodiscard]] inline std::size_t
find_actor(const PeerState& peer, const state::gameplay::physics::Allocation& allocation) noexcept {
    for (std::size_t index = 0; index < peer.actors.size(); ++index) {
        if (peer.actors[index].occupied
            && equal_allocation(peer.actors[index].desired.allocation, allocation)) {
            return index;
        }
    }
    return peer.actors.size();
}

[[nodiscard]] inline const ActorRecord* find_actor_id(const PeerState& peer,
                                                      std::uint64_t actorId) noexcept {
    for (const ActorRecord& actor : peer.actors) {
        if (actor.occupied && actor.desired.allocation.actorId == actorId) {
            return &actor;
        }
    }
    return nullptr;
}

[[nodiscard]] inline bool dependency_ready(const PeerState& peer, std::uint64_t actorId) noexcept {
    if (actorId == state::gameplay::physics::kAbsentActorId) {
        return true;
    }
    const ActorRecord* actor = find_actor_id(peer, actorId);
    return actor != nullptr && actor->createCommitted && !actor->removeRequested;
}

/** @return True while desired or committed dependencies still name the actor. */
[[nodiscard]] inline bool has_live_dependent(const PeerState& peer,
                                             std::uint64_t actorId) noexcept {
    for (const ActorRecord& actor : peer.actors) {
        if (actor.occupied
            && (actor.desired.parentActorId == actorId
                || actor.desired.streamSourceActorId == actorId
                || (actor.createCommitted
                    && (actor.baselineParentActorId == actorId
                        || actor.baselineStreamSourceActorId == actorId)))) {
            return true;
        }
    }
    return false;
}

/** @return True when parent and child contribution ordering is satisfied. */
[[nodiscard]] inline bool
causal_ready(const PeerState& peer, const ActorRecord& actor, Operation operation) noexcept {
    if (operation == Operation::create || operation == Operation::update) {
        return dependency_ready(peer, actor.desired.parentActorId)
               && dependency_ready(peer, actor.desired.streamSourceActorId);
    }
    if (operation == Operation::remove) {
        return !has_live_dependent(peer, actor.desired.allocation.actorId);
    }
    return true;
}

[[nodiscard]] inline bool claim_generation(std::uint64_t& value, std::uint64_t& result) noexcept {
    if (value == kAbsentGeneration || value == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    result = value;
    ++value;
    return true;
}

[[nodiscard]] inline ActorRecord*
resolve_actor(PeerState& peer, std::size_t slot, std::uint64_t generation) noexcept {
    if (!peer.occupied || slot >= peer.actors.size() || generation == kAbsentGeneration) {
        return nullptr;
    }
    ActorRecord& actor = peer.actors[slot];
    return actor.occupied && actor.generation == generation ? &actor : nullptr;
}

[[nodiscard]] inline InFlightRecord* resolve_contribution(PeerState& peer,
                                                          ContributionHandle handle) noexcept {
    if (!peer.occupied || handle.slot >= peer.inFlight.size()
        || handle.peerGeneration != peer.generation || handle.generation == kAbsentGeneration) {
        return nullptr;
    }
    InFlightRecord& stored = peer.inFlight[handle.slot];
    return stored.occupied && stored.generation == handle.generation ? &stored : nullptr;
}

} // namespace sunrise::server::gameplay::physics::replication::detail
