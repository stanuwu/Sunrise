#include "entity_identities.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/gameplay/external/entity_identity_metadata.h"

namespace sunrise::server::gameplay::entity_identities {
namespace {
SRWLOCK g_lock{SRWLOCK_INIT};
identities::Store g_store{};
} // namespace

/** Missing semantic metadata stays unknown while the accepted envelope identity is retained. */
static identities::Observation
make_observation(const middleware::gameplay::external::EntityRecord& record,
                 const state::activity_sdk::Snapshot& catalog,
                 std::uint16_t packetSequence,
                 bool hasPacketSequence,
                 std::uint64_t ordinal,
                 std::uint64_t tick,
                 std::uint16_t recordIndex) noexcept {
    namespace external = middleware::gameplay::external;
    identities::Observation observation{};
    observation.token = {record.token.slot, record.token.incarnation};
    observation.anchor = {record.anchor.slot, record.anchor.incarnation};
    observation.cell = record.cell;
    observation.recordFlags = record.flags;
    observation.trailingState = record.trailingState;
    observation.allocationSequence = record.allocationSequence;
    observation.type = static_cast<std::uint8_t>(record.type);
    observation.anchorChanged = (record.flags & external::entityAnchor) != 0;
    observation.anchorPresent = record.anchorPresent;
    observation.packetSequence = packetSequence;
    observation.hasPacketSequence = hasPacketSequence;
    observation.hasPacketOrdinal = hasPacketSequence;
    observation.packetOrdinal = ordinal;
    observation.packetRecordIndex = recordIndex;
    observation.tick = tick;
    if ((record.flags & external::entityCreate) != 0) {
        observation.action = (record.flags & external::entityRemove) != 0
                                 ? identities::Action::createAndRemove
                                 : identities::Action::create;
        static_cast<void>(
            external::extract_entity_identity_metadata(catalog, record, observation.metadata));
    } else if ((record.flags & external::entityRemove) != 0) {
        observation.action = identities::Action::remove;
    }
    static_cast<void>(external::extract_actor_source_reference(record, observation.actorSource));
    if ((record.flags & external::entityUpdate) != 0
        && (record.flags & external::entityRemove) == 0) {
        observation.damage = record.update.damage;
    }
    return observation;
}

/** Observation logs describe only the outcome of an atomic packet commit. */
static void log_observation(const identities::Source& source,
                            const middleware::gameplay::external::EntityRecord& record,
                            const identities::Observation& observation,
                            identities::Result result) noexcept {
    if (result != identities::Result::updated && result != identities::Result::unchanged) {
        // One name per identities::Result, in declaration order.
        constexpr const char* resultNames[]{"created",
                                            "updated",
                                            "removed",
                                            "unchanged",
                                            "missing",
                                            "stale",
                                            "conflict",
                                            "invalid",
                                            "capacity"};
        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=entity_identity result=%s activity=0x%llX group=0x%llX peer=%llu channel=%llu "
            "view=%llu owner=%llu slot=%u "
            "incarnation=%u type=%u rsat=0x%08X metadata=%u",
            resultNames[static_cast<unsigned>(result)],
            static_cast<unsigned long long>(source.activitySessionId),
            static_cast<unsigned long long>(source.groupSessionId),
            static_cast<unsigned long long>(source.peerGeneration),
            static_cast<unsigned long long>(source.channelGeneration),
            static_cast<unsigned long long>(source.viewGeneration),
            static_cast<unsigned long long>(source.activityClientGeneration),
            record.token.slot,
            static_cast<unsigned>(record.token.incarnation),
            static_cast<unsigned>(record.type),
            observation.metadata.rsatTag,
            static_cast<unsigned>(observation.metadata.hasRsat || observation.metadata.hasSquad
                                  || observation.metadata.hasPlayerBroadcast));
        if (count > 0) {
            core::log::write(
                core::log::Channel::server,
                core::log::Level::debug,
                {line.data(), std::min(static_cast<std::size_t>(count), line.size() - 1)});
        }
    }
}

/** Every accepted record contributes identity evidence after the packet commits. */
void observe(const identities::Source& source,
             const middleware::gameplay::external::EntityBatch& batch,
             const state::activity_sdk::Snapshot& catalog,
             std::uint16_t packetSequence,
             bool hasPacketSequence,
             std::uint64_t ordinal,
             std::uint64_t tick,
             std::uint8_t allocationEpoch,
             bool hasAllocationEpoch,
             std::uint64_t allocationDomain) noexcept {
    namespace external = middleware::gameplay::external;
    const auto count = external::entity_record_count(batch);
    if (count == 0 || count > identities::kObservationBatchCapacity) {
        return;
    }
    std::array<identities::Observation, identities::kObservationBatchCapacity> observations{};
    std::array<identities::Result, identities::kObservationBatchCapacity> results{};
    std::array<std::size_t, identities::kObservationBatchCapacity> retained{};
    std::size_t admitted = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (batch.ignoredRecordMask.test(index)) {
            continue;
        }
        retained[admitted] = index;
        auto& observation = observations[admitted++];
        observation = make_observation(external::entity_record_at(batch, index),
                                       catalog,
                                       packetSequence,
                                       hasPacketSequence,
                                       ordinal,
                                       tick,
                                       static_cast<std::uint16_t>(index));
        observation.allocationEpoch = allocationEpoch;
        observation.hasAllocationEpoch = hasAllocationEpoch;
        observation.allocationDomain = allocationDomain;
    }
    if (admitted == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    const auto result = g_store.observe_batch(
        source, std::span(observations).first(admitted), std::span(results).first(admitted));
    for (std::size_t index = 0; index < admitted; ++index) {
        log_observation(source,
                        external::entity_record_at(batch, retained[index]),
                        observations[index],
                        result == identities::Result::updated ? results[index] : result);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Queries a copied identity under the registry lock. */
identities::Result lookup(const identities::Source& source,
                          identities::Token token,
                          identities::Identity& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto result = g_store.lookup(source, token, output);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/** Enumerates sources only within one exact activity generation. */
std::size_t sources(std::uint64_t activitySessionId,
                    std::uint64_t activityRevision,
                    std::span<identities::Source> output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto count = g_store.sources(activitySessionId, activityRevision, output);
    ReleaseSRWLockShared(&g_lock);
    return count;
}

/** Host advancement preserves every retained row and changes only serial admission. */
bool advance_epoch(const identities::Source& source,
                   std::uint8_t expected,
                   std::uint8_t next,
                   std::uint64_t nextDomain) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const bool advanced = g_store.advance_epoch(source, expected, next, nextDomain);
    ReleaseSRWLockExclusive(&g_lock);
    return advanced;
}

/** A replaced replication view invalidates observations for its group. */
void reset_group(std::uint64_t groupSessionId) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_store.reset_group(groupSessionId);
    ReleaseSRWLockExclusive(&g_lock);
}

/** Removes only the retired peer and view's identity evidence. */
void reset_source(const identities::Source& source) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_store.reset_source(source);
    ReleaseSRWLockExclusive(&g_lock);
}

/** Ends every source lifetime when the transport is replaced or stopped. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_store.reset();
    ReleaseSRWLockExclusive(&g_lock);
}

/** Copies the full source under the registry lock. */
identities::Result snapshot_source(const identities::Source& source,
                                   std::vector<identities::Identity>& output) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto result = g_store.snapshot_source(source, output);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/** Visits the full source under the registry lock; the visitor must not re-enter it. */
identities::Result visit_source(const identities::Source& source,
                                identities::Store::SlotVisitor visitor,
                                void* context) noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto result = g_store.visit_source(source, visitor, context);
    ReleaseSRWLockShared(&g_lock);
    return result;
}
PublicationLease::~PublicationLease() {
    release();
}
/** Delivered retirement tombstones only matching allocations under the registry lock. */
std::size_t retire(const identities::Source& source,
                   std::span<const identities::RetiredLifetime> lifetimes) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    const auto result = g_store.retire(source, lifetimes);
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}
void PublicationLease::release() noexcept {
    if (!held_) {
        return;
    }
    held_ = false;
    ReleaseSRWLockShared(&g_lock);
}
/**
 * Freezes the identity source while the caller validates and copies a publication.
 * @param source Exact source captured by the pending operation.
 * @param output Receives all source slots.
 * @param lease Receives the held lock on success; must be empty on entry.
 * @return Unchanged on success, or the snapshot failure without a held lock.
 */
identities::Result begin_publication(const identities::Source& source,
                                     std::vector<identities::Identity>& output,
                                     PublicationLease& lease) noexcept {
    if (lease.held_) {
        output.clear();
        return identities::Result::invalid;
    }
    AcquireSRWLockShared(&g_lock);
    std::array<identities::Source, identities::kSourceCapacity> sources{};
    const auto count = g_store.sources(source.activitySessionId, source.activityRevision, sources);
    std::size_t matches = 0;
    for (std::size_t index = 0; index < (std::min)(count, sources.size()); ++index) {
        if (sources[index].activityClientGeneration == source.activityClientGeneration) {
            ++matches;
        }
    }
    if (count > sources.size() || matches != 1) {
        output.clear();
        ReleaseSRWLockShared(&g_lock);
        return identities::Result::invalid;
    }
    const auto result = g_store.snapshot_source(source, output);
    if (result == identities::Result::unchanged) {
        lease.held_ = true;
    } else {
        ReleaseSRWLockShared(&g_lock);
    }
    return result;
}
} // namespace sunrise::server::gameplay::entity_identities
