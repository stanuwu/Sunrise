#include "replication_common_reconciler.h"

#include <limits>

namespace sunrise::state::gameplay::external::common_reconciler {

/** Clears every retained identity and generation. */
void Reconciler::reset() noexcept {
    *this = {};
}

/** Opens one exact activity/peer epoch. */
bool Reconciler::open(std::uint64_t activitySessionId,
                      const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
                      std::uint64_t ownerGeneration) noexcept {
    reset();
    if (activitySessionId == 0 || ownerGeneration == 0) {
        return false;
    }
    activitySessionId_ = activitySessionId;
    ownerGeneration_ = ownerGeneration;
    allocationDomain_ = 1;
    patchEpoch_ = {patchEpoch.first, patchEpoch.second};
    phase_ = Phase::awaitingInitial;
    return true;
}

/** Opens from an epoch already authored by this host. */
bool Reconciler::open_known(
    std::uint64_t activitySessionId,
    const middleware::bap::activity_message::patch_epoch::PatchEpoch& patchEpoch,
    std::uint64_t ownerGeneration,
    std::uint8_t replicationEpoch) noexcept {
    if (!open(activitySessionId, patchEpoch, ownerGeneration)) {
        return false;
    }
    initialGeneration_ = replicationEpoch;
    requestedGeneration_ = replicationEpoch;
    phase_ = Phase::ready;
    return true;
}

/** Observes one complete client common root. */
ObserveResult
Reconciler::observe(const middleware::gameplay::external::CommonState& common) noexcept {
    if (phase_ == Phase::closed || phase_ == Phase::failed) {
        return ObserveResult::notOpen;
    }
    const ObserveResult validation = validate(common);
    if (validation != ObserveResult::ready) {
        return validation;
    }

    const std::uint8_t observed = common.entries[0].reconciliationGeneration;
    if (phase_ == Phase::awaitingInitial) {
        initialGeneration_ = observed;
        requestedGeneration_ = static_cast<std::uint8_t>(observed + 1U);
        phase_ = Phase::requestPending;
        return ObserveResult::initialAccepted;
    }
    if (phase_ == Phase::requestPending || phase_ == Phase::awaitingConfirmation) {
        if (observed == initialGeneration_) {
            return ObserveResult::awaitingRequestedGeneration;
        }
        if (phase_ == Phase::awaitingConfirmation && observed == requestedGeneration_) {
            phase_ = Phase::ready;
            return ObserveResult::ready;
        }
        return fail(ObserveResult::unexpectedGeneration);
    }
    if (phase_ == Phase::ready && observed == requestedGeneration_) {
        return ObserveResult::ready;
    }
    if (phase_ == Phase::ready
        && static_cast<std::uint8_t>(requestedGeneration_ - observed)
               <= priorHostGenerationCount_) {
        return ObserveResult::awaitingRequestedGeneration;
    }
    return fail(ObserveResult::unexpectedGeneration);
}

/** Returns the one never-committed message-44 generation that is owed. */
bool Reconciler::pending_request(std::uint8_t& generation) const noexcept {
    generation = 0;
    if (phase_ != Phase::requestPending) {
        return false;
    }
    generation = requestedGeneration_;
    return true;
}

/** Commits the message-44 request after its reliable enqueue succeeds. */
bool Reconciler::commit_request() noexcept {
    if (phase_ != Phase::requestPending) {
        return false;
    }
    phase_ = Phase::awaitingConfirmation;
    return true;
}

/**
 * A host-authored epoch keeps the peer's identity and retained entity state intact.
 * @param expected Epoch preceding the committed host operation.
 * @param next Epoch carried by that operation.
 * @return True only for the next epoch of a ready view.
 */
bool Reconciler::advance_host_epoch(std::uint8_t expected, std::uint8_t next) noexcept {
    if (phase_ != Phase::ready || requestedGeneration_ != expected
        || next != static_cast<std::uint8_t>(expected + 1U)
        || allocationDomain_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (priorHostGenerationCount_ != (std::numeric_limits<std::uint8_t>::max)()) {
        ++priorHostGenerationCount_;
    }
    requestedGeneration_ = next;
    ++allocationDomain_;
    entityEpochConfirmed_ = false;
    firstEntityEpochOrdinal_ = 0;
    return true;
}

/**
 * Untagged packets need an accepted current-epoch packet at an earlier ordinal.
 * @param common Validated common root, or null when omitted from this packet.
 * @param packetOrdinal Expanded packet sequence within the current peer channel.
 * @param hasPacketOrdinal Whether the packet carries a sequence.
 * @return True only when the entity batch belongs to the current allocation domain.
 */
bool Reconciler::qualify_entities(const middleware::gameplay::external::CommonState* common,
                                  std::uint64_t packetOrdinal,
                                  bool hasPacketOrdinal) noexcept {
    if (phase_ != Phase::ready) {
        return false;
    }
    if (common) {
        if (validate(*common) != ObserveResult::ready
            || common->entries[0].reconciliationGeneration != requestedGeneration_) {
            return false;
        }
        if (hasPacketOrdinal) {
            if (!entityEpochConfirmed_ || packetOrdinal < firstEntityEpochOrdinal_) {
                firstEntityEpochOrdinal_ = packetOrdinal;
            }
            entityEpochConfirmed_ = true;
        }
        return true;
    }
    return entityEpochConfirmed_ && hasPacketOrdinal && packetOrdinal >= firstEntityEpochOrdinal_;
}

/** Copies the exact one-entry common root only after the client confirms it. */
bool Reconciler::outbound_common(
    middleware::gameplay::external::CommonState& common) const noexcept {
    common = {};
    if (phase_ != Phase::ready) {
        return false;
    }
    common.patchEpoch = patchEpoch_;
    common.entryCount = 1;
    common.entries[0].activitySessionId = activitySessionId_;
    common.entries[0].reconciliationGeneration = requestedGeneration_;
    return true;
}

/** Returns the current barrier phase. */
Phase Reconciler::phase() const noexcept {
    return phase_;
}

/** Returns the process-local owner generation. */
std::uint64_t Reconciler::owner_generation() const noexcept {
    return ownerGeneration_;
}

/** Returns the first client-selected generation. */
std::uint8_t Reconciler::initial_generation() const noexcept {
    return initialGeneration_;
}

/** Returns the requested generation. */
std::uint8_t Reconciler::requested_generation() const noexcept {
    return requestedGeneration_;
}

/** Validates count, epoch, and session before generation is considered. */
ObserveResult
Reconciler::validate(const middleware::gameplay::external::CommonState& common) noexcept {
    if (common.entryCount != 1) {
        return fail(ObserveResult::wrongShape);
    }
    if (common.patchEpoch != patchEpoch_) {
        return fail(ObserveResult::wrongEpoch);
    }
    if (common.entries[0].activitySessionId != activitySessionId_) {
        return fail(ObserveResult::wrongSession);
    }
    return ObserveResult::ready;
}

/** Moves the barrier to failed and returns the supplied reason. */
ObserveResult Reconciler::fail(ObserveResult reason) noexcept {
    phase_ = Phase::failed;
    return reason;
}

} // namespace sunrise::state::gameplay::external::common_reconciler
