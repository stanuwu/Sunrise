#include <cstddef>
#include <cstdint>

#include "world_coordinator.h"
#include "world_coordinator_internal.h"

namespace sunrise::server::gameplay::physics::replication {
namespace {

namespace external = middleware::gameplay::external;

/** Resolves the only type supported by the empty scriptless payload. */
[[nodiscard]] bool resolve_scriptless_type(const void* context,
                                           const external::EntityToken& token,
                                           external::EntityType& output) noexcept {
    static_cast<void>(context);
    static_cast<void>(token);
    output = external::EntityType::sobject;
    return true;
}

/** Accepts an empty scriptless payload without consuming bits. */
[[nodiscard]] bool read_scriptless_payload(const void* context,
                                           const external::EntityToken& token,
                                           external::EntityType type,
                                           external::TypePayloadPart part,
                                           middleware::encoding::bits::Reader& reader,
                                           external::TypePayload& output) noexcept {
    static_cast<void>(context);
    static_cast<void>(token);
    static_cast<void>(part);
    static_cast<void>(reader);
    if (type != external::EntityType::sobject) {
        return false;
    }
    output = {};
    return true;
}

/** Writes only the empty type-0 fallback and emits no callback bits. */
[[nodiscard]] bool write_scriptless_payload(const void* context,
                                            const external::EntityToken& token,
                                            external::EntityType type,
                                            external::TypePayloadPart part,
                                            const external::TypePayload& payload,
                                            middleware::encoding::bits::Writer& writer) noexcept {
    static_cast<void>(context);
    static_cast<void>(token);
    static_cast<void>(part);
    static_cast<void>(writer);
    return type == external::EntityType::sobject && payload.byteCount == 0;
}

/** @return True when one pending common handle still matches its mutable owner. */
[[nodiscard]] bool pending_common(const common::State& state,
                                  common::CommonContributionHandle handle,
                                  std::uint64_t packetGeneration) noexcept {
    return state.commonPending && handle.stateGeneration == state.generation
           && handle.generation != 0 && handle.generation == state.pendingContributionGeneration
           && handle.packetGeneration == packetGeneration
           && state.pendingPacketGeneration == packetGeneration;
}

/** @return True when one pending entity handle still matches its planner record. */
[[nodiscard]] bool pending_entity(PeerState& peer,
                                  ContributionHandle handle,
                                  std::uint64_t packetGeneration) noexcept {
    const InFlightRecord* stored = detail::resolve_contribution(peer, handle);
    return stored != nullptr && handle.packetGeneration == packetGeneration
           && stored->packetGeneration == packetGeneration;
}

} // namespace

/**
 * Builds one frame and attaches its common/entity owners to a packet.
 * @param peer Exact registered planner object.
 * @param commonState Exact common state for the same ActivityClient lifetime.
 * @param packetGeneration Caller-reserved nonzero packet generation.
 * @param payload Optional type state. Null selects the empty type-0 fallback.
 * @param output Receives the immutable frame and exact pending handles on success.
 * @return Ready, idle, or a fail-closed reason.
 */
FramePrepareResult WorldCoordinator::prepare_frame(PeerState& peer,
                                                   common::State& commonState,
                                                   std::uint64_t packetGeneration,
                                                   const EntityPayloadPlan* payload,
                                                   FrameContribution& output) noexcept {
    const std::size_t peerSlot = find_peer(peer);
    if (!healthy_) {
        return FramePrepareResult::invariantFailure;
    }
    if (!bound_ || packetGeneration == 0 || peerSlot >= peers_.size() || !context_is_current()
        || peers_[peerSlot].planner != &peer || !coordinator_detail::valid_planner(peer)
        || !coordinator_detail::valid_common(commonState, activity_)) {
        return FramePrepareResult::stale;
    }
    if (!common::ready(commonState)) {
        return FramePrepareResult::idle;
    }
    state::gameplay::physics::PeerReplicaSnapshot replica{};
    if (!state::gameplay::physics::snapshot_peer_replica(peer.replica, replica)
        || replica.allocationReferenceCount
               != ::sunrise::server::gameplay::physics::replication::actor_count(peer)
        || replica.contributionTicketCount != contribution_count(peer)) {
        return FramePrepareResult::stale;
    }

    common::CommonPlan commonPlan{};
    const bool commonPresent = common::prepare_common(commonState, commonPlan);
    if (!common::common_covered(commonState) && !commonPresent) {
        return FramePrepareResult::idle;
    }

    Plan entityPlan{};
    const PrepareResult entityResult = prepare_next(peer, entityPlan);
    if (entityResult == PrepareResult::invalid) {
        return FramePrepareResult::invalid;
    }
    const bool entityPresent = entityResult == PrepareResult::ready;
    if (!commonPresent && !entityPresent) {
        return entityResult == PrepareResult::full ? FramePrepareResult::full
                                                   : FramePrepareResult::idle;
    }

    const EntityPayloadPlan fallback{};
    const EntityPayloadPlan& selectedPayload = payload == nullptr ? fallback : *payload;
    external::ExternalEntityFrame frame{};
    if (!build_external_plan(commonPresent ? &commonPlan : nullptr,
                             entityPresent ? &entityPlan : nullptr,
                             entityPresent ? &selectedPayload : nullptr,
                             frame)) {
        return FramePrepareResult::invalid;
    }

    FrameContribution candidate{};
    candidate.frame = frame;
    candidate.coordinatorGeneration = generation_;
    candidate.peerGeneration = peer.generation;
    candidate.packetGeneration = packetGeneration;
    candidate.commonPresent = commonPresent;
    candidate.entityPresent = entityPresent;
    if (commonPresent
        && !common::commit_common(commonState, commonPlan, packetGeneration, candidate.common)) {
        healthy_ = false;
        return FramePrepareResult::invariantFailure;
    }
    if (entityPresent && !commit_plan(peer, entityPlan, packetGeneration, candidate.entity)) {
        const bool rolledBack =
            !commonPresent
            || common::settle_common(
                commonState, candidate.common, packetGeneration, common::CommonOutcome::lost);
        healthy_ = false;
        static_cast<void>(rolledBack);
        return FramePrepareResult::invariantFailure;
    }
    output = candidate;
    return FramePrepareResult::ready;
}

/**
 * Applies one exact external contribution outcome once.
 * @param peer Exact registered planner object.
 * @param commonState Exact common state used by prepare_frame.
 * @param contribution Immutable pending contribution.
 * @param packetGeneration Exact packet generation reported by transport.
 * @param outcome Eligible external contribution result.
 * @return True when every present owner settled once.
 */
bool WorldCoordinator::settle_frame(PeerState& peer,
                                    common::State& commonState,
                                    const FrameContribution& contribution,
                                    std::uint64_t packetGeneration,
                                    Outcome outcome) noexcept {
    const std::size_t peerSlot = find_peer(peer);
    if (!healthy_ || peerSlot >= peers_.size() || contribution.coordinatorGeneration != generation_
        || contribution.peerGeneration != peer.generation || packetGeneration == 0
        || contribution.packetGeneration != packetGeneration
        || (!contribution.commonPresent && !contribution.entityPresent)
        || (outcome != Outcome::success && outcome != Outcome::lost)
        || (contribution.commonPresent
            && !pending_common(commonState, contribution.common, packetGeneration))
        || (contribution.entityPresent
            && !pending_entity(peer, contribution.entity, packetGeneration))) {
        return false;
    }
    state::gameplay::physics::PeerReplicaSnapshot replica{};
    if (!state::gameplay::physics::snapshot_peer_replica(peer.replica, replica)
        || replica.allocationReferenceCount
               != ::sunrise::server::gameplay::physics::replication::actor_count(peer)
        || replica.contributionTicketCount != contribution_count(peer)) {
        return false;
    }

    const common::CommonOutcome commonOutcome =
        outcome == Outcome::success ? common::CommonOutcome::success : common::CommonOutcome::lost;
    if (contribution.commonPresent
        && !common::settle_common(
            commonState, contribution.common, packetGeneration, commonOutcome)) {
        healthy_ = false;
        return false;
    }
    if (contribution.entityPresent
        && !settle(peer, contribution.entity, packetGeneration, outcome)) {
        healthy_ = false;
        return false;
    }
    static_cast<void>(retire_drained());
    return healthy_;
}

/** @return Empty type-0 callbacks for scriptless frame encode and decode. */
const middleware::gameplay::external::TypePayloadCodec& scriptless_payload_codec() noexcept {
    static const middleware::gameplay::external::TypePayloadCodec codec{
        nullptr,
        &resolve_scriptless_type,
        &read_scriptless_payload,
        &write_scriptless_payload,
        0,
        0,
    };
    return codec;
}

} // namespace sunrise::server::gameplay::physics::replication
