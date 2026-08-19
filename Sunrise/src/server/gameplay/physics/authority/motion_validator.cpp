#include "motion_validator.h"

#include <cmath>

namespace sunrise::server::gameplay::physics::authority {
namespace {

[[nodiscard]] bool finite_vector(const world::Vector3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] double length_squared(const world::Vector3& value) noexcept {
    const double x = value.x;
    const double y = value.y;
    const double z = value.z;
    return x * x + y * y + z * z;
}

[[nodiscard]] double distance_squared(const world::Vector3& left,
                                      const world::Vector3& right) noexcept {
    const double x = static_cast<double>(left.x) - static_cast<double>(right.x);
    const double y = static_cast<double>(left.y) - static_cast<double>(right.y);
    const double z = static_cast<double>(left.z) - static_cast<double>(right.z);
    return x * x + y * y + z * z;
}

[[nodiscard]] bool equal_peer(const MotionPeerKey& left, const MotionPeerKey& right) noexcept {
    return left.memberId == right.memberId
           && left.associationGeneration == right.associationGeneration
           && left.channelGeneration == right.channelGeneration
           && left.viewGeneration == right.viewGeneration;
}

[[nodiscard]] bool valid_peer(const MotionPeerKey& peer) noexcept {
    return peer.memberId != 0 && peer.associationGeneration != 0 && peer.channelGeneration != 0
           && peer.viewGeneration != 0;
}

[[nodiscard]] bool valid_limits(const MotionLimits& limits) noexcept {
    return world::valid_blueprint(limits.profile) && limits.maximumTickLag != 0
           && std::isfinite(limits.maximumDisplacementPerTick) && std::isfinite(limits.maximumSpeed)
           && std::isfinite(limits.maximumAccelerationPerTick)
           && std::isfinite(limits.quaternionTolerance) && limits.maximumDisplacementPerTick >= 0.0F
           && limits.maximumSpeed >= 0.0F && limits.maximumAccelerationPerTick >= 0.0F
           && limits.quaternionTolerance >= 0.0F && limits.quaternionTolerance <= 1.0F;
}

[[nodiscard]] bool equal_authority(const world::MotionAuthority& left,
                                   const world::MotionAuthority& right) noexcept {
    return left.ownerMemberId == right.ownerMemberId && left.generation == right.generation
           && left.validFromTick == right.validFromTick && left.mode == right.mode;
}

[[nodiscard]] bool unit_quaternion(const world::Quaternion& value, float tolerance) noexcept {
    const float squared =
        value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    return std::isfinite(squared) && std::fabs(squared - 1.0F) <= tolerance;
}

[[nodiscard]] bool valid_baseline(const MotionBaseline& baseline) noexcept {
    return world::valid_world(baseline.world) && world::valid_actor(baseline.actor)
           && world::finite_transform(baseline.transform) && finite_vector(baseline.linearVelocity)
           && valid_peer(baseline.peer) && valid_limits(baseline.limits)
           && baseline.authority.generation != 0
           && baseline.authority.mode == world::MotionAuthorityMode::clientPredictedHostChecked
           && baseline.authority.ownerMemberId == baseline.peer.memberId;
}

} // namespace

/** Clears every configured client-predicted actor. */
void MotionValidator::reset() noexcept {
    records_ = {};
    size_ = 0;
}

/** Configures or replaces one exact actor generation. */
bool MotionValidator::configure(const MotionBaseline& baseline) noexcept {
    if (!valid_baseline(baseline)) {
        return false;
    }
    if (Record* record = find(baseline.actor); record != nullptr) {
        if (!world::equal_world(record->baseline.world, baseline.world)
            || baseline.tick < record->baseline.tick
            || baseline.authority.generation < record->baseline.authority.generation
            || (baseline.authority.generation == record->baseline.authority.generation
                && !equal_authority(baseline.authority, record->baseline.authority))) {
            return false;
        }
        const bool sameLifetime =
            equal_peer(record->baseline.peer, baseline.peer)
            && record->baseline.authority.generation == baseline.authority.generation;
        const std::uint64_t lastPacketSequence = sameLifetime ? record->lastPacketSequence : 0;
        record->baseline = baseline;
        record->lastPacketSequence = lastPacketSequence;
        return true;
    }
    for (Record& record : records_) {
        if (!record.occupied) {
            record.baseline = baseline;
            record.occupied = true;
            ++size_;
            return true;
        }
    }
    return false;
}

/** Removes one exact actor generation. */
bool MotionValidator::remove(const world::ActorKey& actor) noexcept {
    Record* record = find(actor);
    if (record == nullptr) {
        return false;
    }
    *record = {};
    --size_;
    return true;
}

/** Validates and commits one exact client-predicted motion candidate. */
MotionDecision MotionValidator::evaluate(const MotionCandidate& candidate,
                                         const MotionEvidence& evidence,
                                         MotionResult& result) noexcept {
    result = {};
    Record* record = find(candidate.actor);
    if (record == nullptr) {
        result.decision = MotionDecision::notConfigured;
        return result.decision;
    }
    result.authoritative = record->baseline;
    if (!world::equal_world(candidate.world, record->baseline.world)
        || !equal_peer(candidate.peer, record->baseline.peer)
        || candidate.bubble != record->baseline.bubble) {
        result.decision = MotionDecision::wrongContext;
        return result.decision;
    }
    const world::MotionAuthority& authority = record->baseline.authority;
    if (authority.mode != world::MotionAuthorityMode::clientPredictedHostChecked
        || authority.ownerMemberId != candidate.peer.memberId
        || authority.generation != candidate.authorityGeneration
        || candidate.candidateTick < authority.validFromTick) {
        result.decision = MotionDecision::wrongOwner;
        return result.decision;
    }
    if (candidate.packetSequence == record->lastPacketSequence && candidate.packetSequence != 0) {
        result.decision = MotionDecision::duplicate;
        return result.decision;
    }
    if (candidate.packetSequence == 0 || candidate.packetSequence < record->lastPacketSequence
        || candidate.candidateTick <= record->baseline.tick
        || candidate.candidateTick - record->baseline.tick > record->baseline.limits.maximumTickLag
        || candidate.receiptTick < candidate.candidateTick
        || candidate.receiptTick - candidate.candidateTick
               > record->baseline.limits.maximumTickLag) {
        result.decision = MotionDecision::stale;
        return result.decision;
    }
    if (!world::finite_transform(candidate.transform) || !finite_vector(candidate.linearVelocity)
        || !unit_quaternion(candidate.transform.rotation,
                            record->baseline.limits.quaternionTolerance)) {
        result.decision = MotionDecision::invalid;
        return result.decision;
    }

    const double tickDelta = static_cast<double>(candidate.candidateTick - record->baseline.tick);
    const double displacementLimit =
        static_cast<double>(record->baseline.limits.maximumDisplacementPerTick) * tickDelta;
    const double accelerationLimit =
        static_cast<double>(record->baseline.limits.maximumAccelerationPerTick) * tickDelta;
    const double speedLimit = record->baseline.limits.maximumSpeed;
    if (distance_squared(candidate.transform.position, record->baseline.transform.position)
            > displacementLimit * displacementLimit
        || length_squared(candidate.linearVelocity) > speedLimit * speedLimit
        || distance_squared(candidate.linearVelocity, record->baseline.linearVelocity)
               > accelerationLimit * accelerationLimit) {
        result.decision = MotionDecision::limitExceeded;
        return result.decision;
    }
    if ((record->baseline.limits.requireCollisionFree && !evidence.collisionFree)
        || (record->baseline.limits.requireGrounded && !evidence.grounded)) {
        result.decision = MotionDecision::collisionRejected;
        return result.decision;
    }

    record->baseline.transform = candidate.transform;
    record->baseline.linearVelocity = candidate.linearVelocity;
    record->baseline.tick = candidate.candidateTick;
    record->lastPacketSequence = candidate.packetSequence;
    result.authoritative = record->baseline;
    result.decision = MotionDecision::accepted;
    return result.decision;
}

/** Copies one exact authoritative motion baseline. */
bool MotionValidator::query(const world::ActorKey& actor, MotionBaseline& baseline) const noexcept {
    baseline = {};
    const Record* record = find(actor);
    if (record == nullptr) {
        return false;
    }
    baseline = record->baseline;
    return true;
}

/** Returns the number of configured actor generations. */
std::size_t MotionValidator::size() const noexcept {
    return size_;
}

MotionValidator::Record* MotionValidator::find(const world::ActorKey& actor) noexcept {
    for (Record& record : records_) {
        if (record.occupied && world::equal_actor(record.baseline.actor, actor)) {
            return &record;
        }
    }
    return nullptr;
}

const MotionValidator::Record* MotionValidator::find(const world::ActorKey& actor) const noexcept {
    for (const Record& record : records_) {
        if (record.occupied && world::equal_actor(record.baseline.actor, actor)) {
            return &record;
        }
    }
    return nullptr;
}

} // namespace sunrise::server::gameplay::physics::authority
