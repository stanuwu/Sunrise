#include "activity_roster_push.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/activity_host_control.h"
#include "../../../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../activity/host_runtime.h"
#include "../../../../gameplay/peer/peer_transport.h"
#include "../../../../gameplay/squad_entity_retirement.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::sensor_auth_update;
namespace scriptable = middleware::bap::activity_message::scriptable_auth;

/** No bubble was granted with this body. */
constexpr std::int32_t kNoGrant = -1;
/** The destination name a refusal reports. The selection field is 40 bytes wide. */
constexpr std::size_t kDestinationCapacity = 40;
/** A body over this size is never suppressed, only delivered. */
constexpr std::size_t kBodyRecordCapacity = 16 * 1024;

/** @return Activity Host diagnostic status for one refused roster build. */
[[nodiscard]] server::activity::host::OutputStatus
host_output_status(RosterOutcome outcome) noexcept {
    using Status = server::activity::host::OutputStatus;
    switch (outcome) {
    case RosterOutcome::noEpoch:
        return Status::waitingForEpoch;
    case RosterOutcome::noLayout:
        return Status::noLayout;
    case RosterOutcome::noGroups:
        return Status::noGroups;
    case RosterOutcome::noOverrideTarget:
        return Status::noOverrideTarget;
    case RosterOutcome::published:
    case RosterOutcome::encodeFailed:
    case RosterOutcome::unchanged:
        return Status::frameRefused;
    }
    return Status::frameRefused;
}

/** One outbound body kept for the byte-identical repeat check. */
struct BodyRecord final {
    std::array<std::byte, kBodyRecordCapacity> bytes{};
    std::uint64_t bindingGeneration{};
    std::uint32_t size{};
    bool valid{};
};

/** Per-connection last-body records for the byte-identical repeat check. */
struct ConnectionRecord final {
    BodyRecord rosterSent{};
    BodyRecord rosterStaged{};
    BodyRecord membershipSent{};
    BodyRecord membershipStaged{};
};

// All access runs under the BAP lock, like the Session fields these records extend.
std::array<ConnectionRecord, kSessionCount> g_connectionRecords{};

/** @return This connection's record, or null for an out-of-range connection id. */
[[nodiscard]] ConnectionRecord* connection_record(const Session& session) noexcept {
    return session.id < g_connectionRecords.size() ? &g_connectionRecords[session.id] : nullptr;
}

/** @return True when the record holds this exact body for this exact binding. */
[[nodiscard]] bool matches_record(const BodyRecord& record,
                                  std::uint64_t bindingGeneration,
                                  std::span<const std::byte> body) noexcept {
    return record.valid && record.bindingGeneration == bindingGeneration
           && record.size == body.size()
           && std::equal(body.begin(), body.end(), record.bytes.begin());
}

/** Copies one staged body into a record; an oversized body clears it instead. */
void fill_record(BodyRecord& record,
                 std::uint64_t bindingGeneration,
                 std::span<const std::byte> body) noexcept {
    record.valid = false;
    if (body.size() > record.bytes.size()) {
        return;
    }
    std::copy(body.begin(), body.end(), record.bytes.begin());
    record.bindingGeneration = bindingGeneration;
    record.size = static_cast<std::uint32_t>(body.size());
    record.valid = true;
}

/** Promotes a staged record to the delivered one when its binding still matches. */
void promote_record(BodyRecord& staged,
                    BodyRecord& sent,
                    std::uint64_t bindingGeneration) noexcept {
    if (staged.valid && staged.bindingGeneration == bindingGeneration) {
        sent = staged;
    }
    staged.valid = false;
}

/** Names where this body first differs from the last one delivered on this connection. */
void report_roster_body_delta(const Session& session, std::span<const std::byte> body) noexcept {
    const ConnectionRecord* const record = connection_record(session);
    if (record == nullptr || !record->rosterSent.valid
        || record->rosterSent.bindingGeneration != session.activity.bindingGeneration) {
        return;
    }
    const std::size_t previous = record->rosterSent.size;
    const std::size_t shared = (std::min)(previous, body.size());
    std::size_t offset = 0;
    while (offset < shared && body[offset] == record->rosterSent.bytes[offset]) {
        ++offset;
    }
    if (offset == shared && previous == body.size()) {
        return;
    }
    const unsigned previousByte =
        offset < previous ? std::to_integer<unsigned>(record->rosterSent.bytes[offset]) : 0U;
    const unsigned currentByte =
        offset < body.size() ? std::to_integer<unsigned>(body[offset]) : 0U;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=roster_delta first_byte=%zu bit=%zu "
                                      "bytes=%zu was_bytes=%zu old=0x%02X new=0x%02X",
                                      offset,
                                      offset * 8U,
                                      body.size(),
                                      previous,
                                      previousByte,
                                      currentByte);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** @return True when this body equals the last roster body delivered on this connection. */
bool repeats_delivered_roster_body(const Session& session,
                                   std::span<const std::byte> body) noexcept {
    const ConnectionRecord* const record = connection_record(session);
    return record != nullptr
           && matches_record(record->rosterSent, session.activity.bindingGeneration, body);
}

/** Keeps one staged roster body until its frame outcome is known. */
void stage_roster_body_record(const Session& session, std::span<const std::byte> body) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        fill_record(record->rosterStaged, session.activity.bindingGeneration, body);
    }
}

/** Promotes the staged roster body to the delivered record. */
void commit_roster_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        promote_record(
            record->rosterStaged, record->rosterSent, session.activity.bindingGeneration);
    }
}

/** Drops the staged roster body of a discarded frame. */
void discard_roster_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        record->rosterStaged.valid = false;
    }
}

/** @return True when this body equals the last membership body delivered on this connection. */
bool repeats_delivered_membership_body(const Session& session,
                                       std::span<const std::byte> body) noexcept {
    const ConnectionRecord* const record = connection_record(session);
    return record != nullptr
           && matches_record(record->membershipSent, session.activity.bindingGeneration, body);
}

/** Keeps one staged membership body until its frame outcome is known. */
void stage_membership_body_record(const Session& session,
                                  std::span<const std::byte> body) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        fill_record(record->membershipStaged, session.activity.bindingGeneration, body);
    }
}

/** Promotes the staged membership body to the delivered record. */
void commit_membership_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        promote_record(
            record->membershipStaged, record->membershipSent, session.activity.bindingGeneration);
    }
}

/** Adopts the join burst's staged membership body under the connection's new generation. */
void adopt_join_membership_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record == nullptr || !record->membershipStaged.valid) {
        return;
    }
    // The body was staged before the join commit reserved this generation.
    record->membershipStaged.bindingGeneration = session.activity.bindingGeneration;
    promote_record(
        record->membershipStaged, record->membershipSent, session.activity.bindingGeneration);
}

/** Copies the decode identities from one complete, already-encoded msg-5 roster snapshot. */
bool build_roster_decode_map(const message::Roster& roster,
                             std::uint64_t bindingGeneration,
                             RosterDecodeMap& output) noexcept {
    output = {};
    if (bindingGeneration == 0 || roster.groupCount > roster.groups.size()) {
        return false;
    }

    RosterDecodeMap candidate{};
    candidate.bindingGeneration = bindingGeneration;
    candidate.count = static_cast<std::uint16_t>(roster.groupCount);
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        const message::Group& group = roster.groups[index];
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (candidate.entries[earlier].registryKey == group.key) {
                return false;
            }
        }
        candidate.entries[index] = {.registryKey = group.key, .objectTag = group.objectTag};
    }
    candidate.valid = true;
    output = candidate;
    return true;
}

/** Appends one `sensor_auth_update` svc9 notification carrying the destination's roster. */
bool append_roster_notification(
    Session& session,
    Scratch& scratch,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written,
    const middleware::bap::activity_message::patch_epoch::PatchEpoch* epoch,
    const EffectiveRegion* exactRegion,
    bool solicited,
    const RefreshReport* refresh,
    bool allowEntityRetirement) noexcept {
    if (written > response.size()) {
        return false;
    }
    const auto initialLeases = session.activityRosterGroupLeases;
    const bool initialRosterOwedForEpoch = session.activityRosterOwedForEpoch;
    const std::uint8_t initialRosterSends = session.activityRosterSends;
    const std::uint8_t initialRosterState = session.activityRosterState;
    const std::uint8_t initialRegionEpoch = session.activityRosterRegionEpoch;
    const std::int32_t initialRegionBubble = session.activityRosterRegionBubble;
    server::activity::host::AuthState hostState{};
    const bool hasHostState =
        server::activity::host::auth_state(session.activity.session, hostState);
    const bool hostStatePending = hasHostState && hostState.revision != 0
                                  && hostState.revision != session.activityHostStateRevision;
    server::activity::host::PendingScriptableOverride scriptablePending{};
    const bool hasScriptablePending =
        server::activity::host::pending_scriptable_override_for_activity_client(
            session.activity.session, session.activity.bindingGeneration, scriptablePending);
    const bool singleScriptableLink =
        !hasScriptablePending || activity_link_count_locked(session.activity.session) == 1;
    const bool squadPending =
        hasScriptablePending && singleScriptableLink
        && scriptablePending.kind == server::activity::host::ScriptableOverrideKind::squad;
    // A lifetime request changes the type-17 state the builder writes; it substitutes no slot body.
    const bool lifetimePending =
        hasScriptablePending && singleScriptableLink
        && scriptablePending.kind == server::activity::host::ScriptableOverrideKind::lifetime;
    // The loading lifetime is the presentation, not the spawn hold: state 4 shows the loading
    // screen and refuses the native spawn gate on its own, so it is released once the region is
    // instantiated. `awaiting_client_sync` carries the hold on to the client's arrival report.
    // An explicit lifetime request still wins.
    const bool clientLoading = !client_region_ready(session, refresh);
    const bool bodyPending = hasScriptablePending && singleScriptableLink && !lifetimePending;
    // Bodies committed behind the head share its push, so they are installed on this same body.
    std::array<server::activity::host::PendingScriptableOverride,
               server::activity::host::kPendingScriptableTailCapacity>
        tailPending{};
    std::array<TailAuthOverride, server::activity::host::kPendingScriptableTailCapacity>
        tailOverrides{};
    std::size_t tailCount = 0;
    if (bodyPending) {
        const std::size_t queued = server::activity::host::pending_scriptable_tail(
            session.activity.session, std::span(tailPending));
        for (std::size_t index = 0; index < queued; ++index) {
            const server::activity::host::PendingScriptableOverride& source = tailPending[index];
            if (source.byteCount == 0 || source.byteCount > source.body.size()) {
                continue;
            }
            TailAuthOverride& target = tailOverrides[tailCount];
            std::copy(source.body.begin(), source.body.end(), target.value.body.begin());
            target.value.objectTag = source.target.objectTag;
            target.value.key = source.target.registryKey;
            target.value.authSchema = source.target.authSchema;
            target.value.slotIndex = source.target.slotIndex;
            target.value.bitCount = source.bitCount;
            target.value.slotType = source.target.slotType;
            target.value.byteCount = source.byteCount;
            target.value.sdkCompiled = source.sdkCompiled;
            target.value.present = true;
            target.rosterGroupIndex = source.target.rosterGroupIndex;
            target.rosterSlotOffset = source.target.rosterSlotOffset;
            target.stateLocalRosterTarget = source.target.stateLocalRoster;
            ++tailCount;
        }
    }
    message::AuthOverride authOverride{};
    if (bodyPending) {
        std::copy(scriptablePending.body.begin(),
                  scriptablePending.body.end(),
                  authOverride.body.begin());
        authOverride.objectTag = scriptablePending.target.objectTag;
        authOverride.key = scriptablePending.target.registryKey;
        authOverride.authSchema = scriptablePending.target.authSchema;
        authOverride.slotIndex = scriptablePending.target.slotIndex;
        authOverride.bitCount = scriptablePending.bitCount;
        authOverride.slotType = scriptablePending.target.slotType;
        authOverride.byteCount = scriptablePending.byteCount;
        authOverride.sdkCompiled = scriptablePending.sdkCompiled;
        authOverride.present = true;
    } else if (hasScriptablePending && !singleScriptableLink) {
        server::activity::host::note_scriptable_attempt(
            session.activity.session,
            session.activity.bindingGeneration,
            scriptablePending,
            server::activity::host::OutputStatus::ambiguousLinks);
    }
    message::Snapshot snapshot{};
    std::array<char, kDestinationCapacity> destination{};
    std::size_t destinationLength = 0;
    RosterOutcome outcome = RosterOutcome::noEpoch;
    // A caller answering message 52 supplies that message's epoch, which is the same proof of a
    // live epoch the connection field gives once it is published.
    const bool hasEpoch =
        epoch != nullptr
        || (session.activityPatchEpoch.seen
            && session.activityPatchEpoch.bindingGeneration == session.activity.bindingGeneration);
    // Without the epoch the body cannot be built at all, so any answer owed here is deferred until
    // the client reports one. Message 52 is that report.
    session.activityRosterOwedForEpoch = session.activityRosterOwedForEpoch || !hasEpoch;
    if (hasEpoch) {
        outcome = build_roster_snapshot(
            session,
            scratch,
            snapshot,
            destination,
            destinationLength,
            epoch,
            lifetimePending ? scriptablePending.lifetimeState
            : clientLoading ? server::activity::host::kLoadingLifetimeState
            : hasHostState  ? hostState.lifetimeState
                            : server::activity::host::kDefaultLifetimeState,
            bodyPending ? &authOverride : nullptr,
            scriptablePending.target.rosterGroupIndex,
            scriptablePending.target.rosterSlotOffset,
            scriptablePending.target.stateLocalRoster,
            scriptablePending.target.stateLocalRegion,
            scriptablePending.target.sdkObjectIndex,
            scriptablePending.target.stateLocalRoster ? &scriptablePending.stateLocalRosterGroup
                                                      : nullptr,
            exactRegion,
            refresh,
            std::span(tailOverrides).first(tailCount));
    }
    const std::string_view name(destination.data(), destinationLength);
    if (outcome != RosterOutcome::published) {
        report_roster_push(session, snapshot, name, 0, kNoGrant, outcome, 0, 0);
        if (hostStatePending) {
            server::activity::host::note_auth_attempt(session.activity.session,
                                                      session.activity.bindingGeneration,
                                                      hostState.revision,
                                                      hostState.lifetimeState,
                                                      host_output_status(outcome));
        }
        if (hasScriptablePending && singleScriptableLink) {
            server::activity::host::note_scriptable_attempt(session.activity.session,
                                                            session.activity.bindingGeneration,
                                                            scriptablePending,
                                                            host_output_status(outcome));
        }
        return false;
    }

    // The grant is picked here and committed only once the frame reaches the caller, so a
    // discarded body leaves the bubble ungranted and the next push retries it.
    state::activity::bubble_authority::Grant grant{};
    // Authority is granted for the bubble the client is heading to, so it is in place before the
    // slice-set switch, and for the one it is in otherwise. The wire bubble is unsigned, then
    // checked as signed; an out-of-range value becomes negative and fails.
    const std::int32_t pendingRegion =
        state::activity::membership::reported_region(session.activity.session.sessionId);
    const bool enteringBubble = session.activityRosterRegionBubble >= 0
                                && session.activityRosterRegionBubble != initialRegionBubble;
    const std::int32_t grantRegion =
        enteringBubble
            ? static_cast<std::int32_t>(snapshot.region)
            : (pendingRegion >= 0 ? pendingRegion : static_cast<std::int32_t>(snapshot.region));
    if (state::activity::bubble_authority::select_grant(
            session.activity.session.sessionId, grantRegion, grant, enteringBubble)) {
        snapshot.hasGrant = true;
        snapshot.grant.bubble = grant.bubble;
        snapshot.grant.token = grant.token;
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    server::gameplay::squad_entity_retirement::RetirementPlan entityRetirement{};
    const auto retirementPriorEpoch = session.activity.replicationEpoch;
    const auto& epochRequest = session.activityReplicationEpoch;
    const auto retirementBaseEpoch =
        epochRequest.staged && epochRequest.bindingGeneration == session.activity.bindingGeneration
            ? epochRequest.generation
            : retirementPriorEpoch;
    const auto retirementEpoch = static_cast<std::uint8_t>(retirementBaseEpoch + 1U);
    std::size_t messageSize = 0;
    RosterDecodeMap decodeMap{};
    const MissionSeedLease& stagedMissionSeed = session.activityMissionSeed;
    // During the region-arrival window the pending revision's content is deliberately not in
    // this body, so the pending revision neither forces a send nor commits as published.
    const bool missionSeedPending =
        stagedMissionSeed.configured
        && stagedMissionSeed.bindingGeneration == session.activity.bindingGeneration
        && stagedMissionSeed.revision != stagedMissionSeed.publishedRevision
        && !stagedMissionSeed.regionArrivalPending;
    bool encoded = message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize);
    // An unsolicited body identical to the last delivered one is skipped. A solicited one never
    // is. The repeat check knows only this host's own history, and a slice-set teardown clears
    // the client's mirror without telling us, which is exactly when it asks again.
    const bool suppressible = encoded && !solicited && !snapshot.hasGrant && !hostStatePending
                              && !(hasScriptablePending && singleScriptableLink)
                              && !missionSeedPending;
    // Which terms held is in the log line, because a repeat that one of them forced reaches the
    // client as a fresh apply.
    const std::uint8_t forced = static_cast<std::uint8_t>(
        (solicited ? kRosterForceSolicited : 0U) | (snapshot.hasGrant ? kRosterForceGrant : 0U)
        | (hostStatePending ? kRosterForceHostState : 0U)
        | (hasScriptablePending && singleScriptableLink ? kRosterForceScriptable : 0U)
        | (missionSeedPending ? kRosterForceMissionSeed : 0U));
    const std::uint64_t bodyHash =
        encoded ? body_hash(std::span(scratch.responseBody).first(messageSize)) : 0;
    // Two full bodies a couple of seconds apart make the client rebuild the world twice, and
    // every field the roster log prints is identical across them. This names the byte that moved.
    if (encoded) {
        report_roster_body_delta(session, std::span(scratch.responseBody).first(messageSize));
    }
    if (suppressible
        && repeats_delivered_roster_body(session,
                                         std::span(scratch.responseBody).first(messageSize))) {
        SecureZeroMemory(scratch.responseBody.data(), messageSize);
        SecureZeroMemory(&initialNonce, sizeof initialNonce);
        // A skipped send spends nothing.
        session.activityRosterGroupLeases = initialLeases;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
        session.activityRosterRegionEpoch = initialRegionEpoch;
        session.activityRosterRegionBubble = initialRegionBubble;
        report_roster_push(
            session, snapshot, name, 0, kNoGrant, RosterOutcome::unchanged, bodyHash, forced);
        return false;
    }
    if (encoded && snapshot.hasGrant && enteringBubble && allowEntityRetirement
        && server::gameplay::squad_entity_retirement::prepare_retirement(
            session.activity.session,
            session.activity.bindingGeneration,
            grant.bubble,
            entityRetirement)) {
        namespace control = middleware::bap::activity_message::host_control;
        const control::PurgeAuthorityBody retirement{
            .slots = entityRetirement.entities, .epoch = retirementEpoch, .reason = 0};
        std::array<std::byte, control::kPurgeAuthorityByteCount> retirementBytes{};
        std::size_t retirementSize{};
        encoded = control::encode_purge_authority(retirement, retirementBytes, retirementSize)
                  && append_notification_frame(scratch,
                                               session.activity.session.sessionId,
                                               control::kPurgeAuthorityMessageType,
                                               std::span(retirementBytes).first(retirementSize),
                                               key,
                                               nonce,
                                               response,
                                               written);
        if (encoded) middleware::secure_channel::advance_nonce(nonce);
    }
    encoded = encoded
              && append_notification_frame(scratch,
                                           session.activity.session.sessionId,
                                           message::kMessageType,
                                           std::span(scratch.responseBody).first(messageSize),
                                           key,
                                           nonce,
                                           response,
                                           written);
    // The frame contains a complete roster, not a delta against this host map. Capture the exact
    // group order only after both encoders accept it. A malformed identity map refuses the whole
    // attempt, and the rollback below removes the staged bytes and counters.
    encoded =
        encoded
        && build_roster_decode_map(snapshot.roster, session.activity.bindingGeneration, decodeMap);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        // The deferred answer is discharged by the body that carries it.
        session.activityRosterOwedForEpoch = false;
        // Staged, not published. The grant and the counters are one-way and this body may still be
        // discarded, so they are held here and settled by `commit_staged_roster` or
        // `discard_staged_roster`.
        session.activityRosterStaged.grant = grant;
        session.activityRosterStaged.entityRetirement = entityRetirement;
        session.activityRosterStaged.retirementPriorEpoch = retirementPriorEpoch;
        session.activityRosterStaged.retirementBaseEpoch = retirementBaseEpoch;
        session.activityRosterStaged.retirementEpoch = retirementEpoch;
        session.activityRosterStaged.decodeMap = decodeMap;
        session.activityRosterStaged.bindingGeneration = session.activity.bindingGeneration;
        session.activityRosterStaged.priorLeases = initialLeases;
        session.activityRosterStaged.priorRosterOwedForEpoch = initialRosterOwedForEpoch;
        session.activityRosterStaged.priorSends = initialRosterSends;
        session.activityRosterStaged.priorState = initialRosterState;
        session.activityRosterStaged.priorRegionEpoch = initialRegionEpoch;
        session.activityRosterStaged.priorRegionBubble = initialRegionBubble;
        session.activityRosterStaged.hostStateRevision = hostState.revision;
        session.activityRosterStaged.hostLifetimeState = snapshot.lifetime;
        const MissionSeedLease& missionSeed = session.activityMissionSeed;
        session.activityRosterStaged.missionSeedRevision = missionSeed.revision;
        session.activityRosterStaged.scriptableOverride = scriptablePending;
        session.activityRosterStaged.hasGrant = snapshot.hasGrant;
        session.activityRosterStaged.hasHostState = hostStatePending;
        session.activityRosterStaged.hasMissionSeedRevision =
            missionSeed.configured
            && missionSeed.bindingGeneration == session.activity.bindingGeneration
            && missionSeed.revision != missionSeed.publishedRevision
            && !missionSeed.regionArrivalPending;
        session.activityRosterStaged.hasScriptableOverride =
            hasScriptablePending && singleScriptableLink;
        session.activityRosterStaged.stateLocalRegion = scriptablePending.target.stateLocalRegion;
        session.activityRosterStaged.activatesSquadOverride = squadPending;
        if (squadPending && scriptablePending.target.stateLocalRoster
            && snapshot.roster.groupCount != 0) {
            for (std::size_t index = 0; index < snapshot.roster.groupCount; ++index) {
                const message::Group& generated = snapshot.roster.groups[index];
                if (generated.objectTag != scriptablePending.target.objectTag
                    || generated.key != scriptablePending.target.registryKey) {
                    continue;
                }
                session.activityRosterStaged.squadStateSequence = generated.stateSequence;
                session.activityRosterStaged.hasSquadStateSequence = generated.hasStateSequence;
                break;
            }
        }
        session.activityRosterStaged.staged = true;
        stage_roster_body_record(session, std::span(scratch.responseBody).first(messageSize));
    }
    report_roster_push(session,
                       snapshot,
                       name,
                       encoded ? messageSize : 0,
                       encoded && snapshot.hasGrant ? snapshot.grant.bubble : kNoGrant,
                       encoded ? RosterOutcome::published : RosterOutcome::encodeFailed,
                       bodyHash,
                       forced);
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    if (!encoded) {
        if (hostStatePending) {
            server::activity::host::note_auth_attempt(
                session.activity.session,
                session.activity.bindingGeneration,
                hostState.revision,
                hostState.lifetimeState,
                server::activity::host::OutputStatus::frameRefused);
        }
        if (hasScriptablePending && singleScriptableLink) {
            server::activity::host::note_scriptable_attempt(
                session.activity.session,
                session.activity.bindingGeneration,
                scriptablePending,
                server::activity::host::OutputStatus::frameRefused);
        }
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
        session.activityRosterGroupLeases = initialLeases;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
        session.activityRosterRegionEpoch = initialRegionEpoch;
        session.activityRosterRegionBubble = initialRegionBubble;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

bool validate_staged_roster(const Session& session) noexcept {
    const auto& staged = session.activityRosterStaged;
    return !staged.staged || !staged.entityRetirement.pending
           || (staged.bindingGeneration == session.activity.bindingGeneration
               && staged.retirementPriorEpoch == session.activity.replicationEpoch
               && server::gameplay::squad_entity_retirement::validate_retirement(
                   session.activity.session,
                   session.activity.bindingGeneration,
                   staged.entityRetirement));
}

/** Retained identities cannot change between final validation and the caller copy. */
bool begin_staged_roster_publication(
    const Session& session, server::gameplay::entity_identities::PublicationLease& lease) noexcept {
    const auto& staged = session.activityRosterStaged;
    return !staged.staged || !staged.entityRetirement.pending
           || (staged.bindingGeneration == session.activity.bindingGeneration
               && staged.retirementPriorEpoch == session.activity.replicationEpoch
               && server::gameplay::squad_entity_retirement::begin_retirement_publication(
                   session.activity.session,
                   session.activity.bindingGeneration,
                   staged.entityRetirement,
                   lease));
}

/** Settles a staged roster body that reached the caller. */
void commit_staged_roster(Session& session) noexcept {
    if (!session.activityRosterStaged.staged) {
        return;
    }
    if (session.activityRosterStaged.bindingGeneration != session.activity.bindingGeneration) {
        session.activityRosterStaged = {};
        discard_roster_body_record(session);
        return;
    }
    commit_roster_body_record(session);
    // The BAP lock serializes publication and incoming activity messages, so replacing the whole
    // fixed map here exposes either the prior delivered roster or this complete delivered roster.
    session.activityRosterDecode = session.activityRosterStaged.decodeMap;
    if (session.activityRosterStaged.hasGrant) {
        if (session.activityRosterStaged.entityRetirement.pending) {
            server::gameplay::squad_entity_retirement::commit_retirement(
                session.activityRosterStaged.entityRetirement);
            const auto& staged = session.activityRosterStaged;
            session.activity.replicationEpoch = staged.retirementEpoch;
            if (staged.retirementBaseEpoch != staged.retirementPriorEpoch)
                static_cast<void>(server::gameplay::peer::commit_replication_epoch(
                    session.activity.session,
                    session.activity.bindingGeneration,
                    staged.retirementPriorEpoch,
                    staged.retirementBaseEpoch));
            static_cast<void>(
                server::gameplay::peer::commit_replication_epoch(session.activity.session,
                                                                 session.activity.bindingGeneration,
                                                                 staged.retirementBaseEpoch,
                                                                 staged.retirementEpoch));
            state::activity::bubble_authority::record_purge(session.activity.session.sessionId,
                                                            staged.entityRetirement.entities);
            auto& request = session.activityReplicationEpoch;
            if (request.bindingGeneration == session.activity.bindingGeneration
                && (request.generation == staged.retirementBaseEpoch
                    || request.generation == staged.retirementEpoch)) {
                request.pending = false;
                request.staged = false;
            }
        }
        state::activity::bubble_authority::record_grant(session.activity.session.sessionId,
                                                        session.activityRosterStaged.grant);
    }
    if (session.activityRosterStaged.hasHostState) {
        session.activityHostStateRevision = session.activityRosterStaged.hostStateRevision;
        server::activity::host::note_auth_transport_staged(
            session.activity.session,
            session.activity.bindingGeneration,
            session.activityRosterStaged.hostStateRevision,
            session.activityRosterStaged.hostLifetimeState);
    }
    if (session.activityRosterStaged.hasMissionSeedRevision) {
        MissionSeedLease& missionSeed = session.activityMissionSeed;
        if (missionSeed.configured
            && missionSeed.bindingGeneration == session.activity.bindingGeneration
            && missionSeed.revision == session.activityRosterStaged.missionSeedRevision) {
            missionSeed.publishedRevision = missionSeed.revision;
        }
    }
    bool scriptableCommitReady = session.activityRosterStaged.hasScriptableOverride;
    if (scriptableCommitReady && session.activityRosterStaged.activatesSquadOverride) {
        scriptableCommitReady = activate_staged_squad_override(session);
    }
    if (scriptableCommitReady) {
        server::activity::host::note_scriptable_transport_staged(
            session.activity.session,
            session.activity.bindingGeneration,
            session.activityRosterStaged.scriptableOverride);
    } else if (session.activityRosterStaged.hasScriptableOverride) {
        server::activity::host::note_scriptable_attempt(
            session.activity.session,
            session.activity.bindingGeneration,
            session.activityRosterStaged.scriptableOverride,
            server::activity::host::OutputStatus::noOverrideTarget);
    }
    session.activityRosterStaged = {};
}

/** Puts back what a staged roster body advanced, now that the body has been discarded. */
void discard_staged_roster(Session& session) noexcept {
    if (!session.activityRosterStaged.staged) {
        return;
    }
    if (session.activityRosterStaged.hasHostState) {
        server::activity::host::note_auth_attempt(
            session.activity.session,
            session.activity.bindingGeneration,
            session.activityRosterStaged.hostStateRevision,
            session.activityRosterStaged.hostLifetimeState,
            server::activity::host::OutputStatus::frameRefused);
    }
    if (session.activityRosterStaged.hasScriptableOverride) {
        server::activity::host::note_scriptable_attempt(
            session.activity.session,
            session.activity.bindingGeneration,
            session.activityRosterStaged.scriptableOverride,
            server::activity::host::OutputStatus::frameRefused);
    }
    // The client never saw this body, so its state byte must not be spent. The next push has to
    // move the byte again or the client does not rebuild its roster objects.
    discard_roster_body_record(session);
    rollback_staged_roster_state(session);
}

} // namespace sunrise::server::bap::encrypted::push::activity
