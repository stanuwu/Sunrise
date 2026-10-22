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
#include "../../../../../middleware/bap/activity_message/entity_slots.h"
#include "../../../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../activity/host_runtime.h"
#include "../../../../gameplay/peer/peer_transport.h"
#include "../../../../gameplay/squad_entity_retirement.h"
#include "../../../activity_mission_seed_lease.h"
#include "activity_member_departure_push.h"
#include "activity_member_roster.h"
#include "activity_notification_frame.h"
#include "activity_roster_device_publication.h"
#include "activity_roster_grant.h"
#include "internal.h"
#include "mission_entity_allocation.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::sensor_auth_update;

/** No bubble was granted with this body. */
constexpr std::int32_t kNoGrant = -1;
/** The destination name a refusal reports. The selection field is 40 bytes wide. */
constexpr std::size_t kDestinationCapacity = 40;
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

/** @return True when the body writes the participation record, the only field with the hold. */
[[nodiscard]] bool carries_participation(const message::Roster& roster) noexcept {
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        const message::Group& group = roster.groups[index];
        if (!group.retired && group.key == roster.playerKeyGroup) {
            return true;
        }
    }
    return false;
}

} // namespace

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
    bool allowEntityRetirement,
    bool peerLeave) noexcept {
    // The client reported it is leaving and the leave delta already went out on this link.
    const bool left = session.activityLeftGeneration != 0
                      && session.activityLeftGeneration == session.activity.bindingGeneration;
    if (written > response.size() || left) {
        return false;
    }
    const auto initialLeases = session.activityRosterGroupLeases;
    const bool initialRosterOwedForEpoch = session.activityRosterOwedForEpoch;
    const std::uint8_t initialRosterSends = session.activityRosterSends;
    const std::uint8_t initialRosterState = session.activityRosterState;
    const std::uint8_t initialRegionEpoch = session.activityRosterRegionEpoch;
    const std::int32_t initialRegionBubble = session.activityRosterRegionBubble;
    const bool placedRetirementPending =
        server::gameplay::squad_entity_retirement::placed_transition_pending(
            session.activity.session, session.activity.bindingGeneration);
    server::activity::host::AuthState hostState{};
    const bool hasHostState =
        server::activity::host::auth_state(session.activity.session, hostState);
    // A link's first roster since it joined carries no line said before it: the cursor starts at
    // the newest pulse, so only the pulses retained after this body are owed to the link.
    if (hasHostState && session.activityRosterSends == 0) {
        session.activityDialogueRevision = hostState.dialogueRevision;
    }
    const bool hostStatePending = hasHostState && hostState.revision != 0
                                  && hostState.revision != session.activityHostStateRevision;
    server::activity::host::PendingScriptableOverride scriptablePending{};
    const bool hasScriptablePending =
        server::activity::host::pending_scriptable_override_for_activity_client(
            session.activity.session, session.activity.bindingGeneration, scriptablePending);
    const bool singleScriptableLink =
        !hasScriptablePending
        || activity_link_count_locked(session.activity.session, session.activity.bindingGeneration)
               == 1;
    // A leave delta retires every group, so it carries no host state and no typed body. Both stay
    // owed and neither is spent on it.
    const bool ownsPending = hasScriptablePending && singleScriptableLink && !peerLeave;
    const bool ownsHostState = hostStatePending && !peerLeave;
    // The estate this body carries holds a body another link's push staged, and the client
    // behind this link applies it itself; only a push lets that client see it. The push that
    // staged it carried it as the pending body, so that link's cursor moves with its commit. The
    // term owes the push, not a resend: a body identical to the last delivered one is still
    // skipped, and the cursors move with the skip.
    const bool estatePending = hasHostState && !peerLeave
                               && session.activity.role == ActivityClientRole::privateCurrent
                               && hostState.estateRevision != session.activityEstateRevision;
    const bool squadPending =
        ownsPending
        && scriptablePending.kind == server::activity::host::ScriptableOverrideKind::squad;
    // A lifetime request changes the type-17 state the builder writes; it substitutes no slot body.
    const bool lifetimePending =
        ownsPending
        && scriptablePending.kind == server::activity::host::ScriptableOverrideKind::lifetime;
    // State 4 shows the loading screen and releases once the region is instantiated. The spawn
    // hold is `awaiting_client_sync`, not the lifetime. An explicit lifetime request still wins.
    const bool clientLoading = !client_region_ready(session, refresh);
    // No unsolicited msg 5 while the client loads: with no slice set current it rewrites the live
    // presence mask and relinks sense records. The region report is answered solicited. Only the
    // private link's session sees the placement, so only it holds.
    if (clientLoading && !solicited && !peerLeave
        && session.activity.role == ActivityClientRole::privateCurrent) {
        report_roster_deferral(session,
                               hostStatePending ? hostState.revision : 0,
                               hasScriptablePending ? scriptablePending.revision : 0);
        return false;
    }
    const bool bodyPending = ownsPending && !lifetimePending;
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
            target.value.originatingHostRevision = source.revision;
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
        authOverride.originatingHostRevision = scriptablePending.revision;
    } else if (hasScriptablePending && !singleScriptableLink && !peerLeave) {
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
        if (ownsHostState) {
            server::activity::host::note_auth_attempt(session.activity.session,
                                                      session.activity.bindingGeneration,
                                                      hostState.revision,
                                                      hostState.lifetimeState,
                                                      host_output_status(outcome));
        }
        if (ownsPending) {
            server::activity::host::note_scriptable_attempt(session.activity.session,
                                                            session.activity.bindingGeneration,
                                                            scriptablePending,
                                                            host_output_status(outcome));
        }
        return false;
    }

    // The leave delta keeps every key in the wire array and clears its presence bit. The client
    // then deactivates each row while its owner is still valid and unregisters it cleanly.
    std::size_t retiredGroups = 0;
    if (peerLeave) {
        retire_member_roster(snapshot);
        retiredGroups = snapshot.roster.groupCount;
    }
    // The grant is picked here and committed only once the frame reaches the caller, so a
    // discarded body leaves the bubble ungranted and the next push retries it.
    state::activity::bubble_authority::Grant grant{};
    const bool enteringBubble = session.activityRosterRegionBubble >= 0
                                && session.activityRosterRegionBubble != initialRegionBubble;
    // The held region comes from the report being answered, because a switch report's body
    // publishes the region behind the player. The wire bubble is unsigned, then checked as signed;
    // an out-of-range value becomes negative and fails.
    const bool sharedBubbles = activity_link_count_locked(session.activity.session)
                               >= state::activity::bubble_authority::kSharedLinkCount;
    const std::int32_t pendingRegion =
        state::activity::membership::reported_region(session.activity.session.sessionId);
    const GrantRequest grantRequest =
        grant_request({.sharedBubbles = sharedBubbles,
                       .regionReady = client_region_ready(session, refresh),
                       .heldRegion = state::activity::membership::instantiated_region(
                           client_placement(session, refresh)),
                       .enteringBubble = enteringBubble,
                       .bodyRegion = static_cast<std::int32_t>(snapshot.region),
                       .pendingRegion = pendingRegion});
    // Links that share a session also ask who released the bubble; a sole link keeps its rule.
    const std::uint64_t sessionId = session.activity.session.sessionId;
    // Read before the grant commits, which clears the bubble's releaser.
    std::uint64_t releaser = 0;
    HandoverSquadSense handedSquads{};
    if (!peerLeave && !placedRetirementPending
        && (sharedBubbles
                ? state::activity::bubble_authority::select_shared_grant(
                      sessionId,
                      session.activity.bindingGeneration,
                      grantRequest.region,
                      pendingRegion,
                      enteringBubble,
                      grant)
                : state::activity::bubble_authority::select_grant(
                      sessionId, grantRequest.region, grant, grantRequest.renewReleased))) {
        snapshot.hasGrant = true;
        snapshot.grant.bubble = grant.bubble;
        snapshot.grant.token = grant.token;
        // A released bubble, abdicated or left, resumes from its newest squad Sense: the previous
        // holder's, or this link's own when it walks back into an encounter it cleared.
        state::activity::bubble_authority::GrantHolders releasers{};
        state::activity::bubble_authority::grant_releasers(sessionId, releasers);
        releaser = grant.bubble < releasers.size() ? releasers[grant.bubble] : 0;
        if (releaser != 0) {
            append_handover_squad_sense(
                session, scratch, snapshot, grant.bubble, releaser, handedSquads);
        }
    }

    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    server::gameplay::squad_entity_retirement::RetirementPlan entityRetirement{};
    std::uint64_t retirementSequence{};
    const bool epochCurrent =
        state::activity::replication_sequence(session.activity.session, retirementSequence)
        && retirementSequence == session.activity.replicationSequence
        && retirementSequence != (std::numeric_limits<std::uint64_t>::max)();
    const auto retirementPriorEpoch = session.activity.replicationEpoch;
    const auto retirementBaseEpoch = retirementPriorEpoch;
    const auto retirementEpoch = static_cast<std::uint8_t>(retirementBaseEpoch + 1U);
    std::size_t messageSize = 0;
    RosterDecodeMap decodeMap{};
    const MissionSeedLease& stagedMissionSeed = session.activityMissionSeed;
    // During the region-arrival window the pending revision's content is deliberately not in
    // this body, so the pending revision neither forces a send nor commits as published.
    const bool missionSeedPending =
        mission_seed_publication_pending(stagedMissionSeed, session.activity.bindingGeneration);
    state::activity::entity_slots::PendingMutation entitySlotAllocation{};
    const bool allocationOwed = mission_entity_allocation_owed(session, squadPending);
    // Client reports commit their own State revision after this builder. Publish the dependent
    // allocation and roster together on the next host push, against committed client state.
    if (allocationOwed && solicited) {
        session.activityRosterGroupLeases = initialLeases;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
        session.activityRosterRegionEpoch = initialRegionEpoch;
        session.activityRosterRegionBubble = initialRegionBubble;
        SecureZeroMemory(&initialNonce, sizeof initialNonce);
        return false;
    }
    bool encoded = message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize);
    encoded =
        encoded
        && (!allocationOwed || prepare_mission_entity_allocation(session, entitySlotAllocation));
    // A renewal retires what this link released in the bubble it renews. That is the granted
    // bubble, which a switch report's body region does not name once links share a session.
    const auto retirementBubble =
        snapshot.hasGrant
            ? grant.bubble
            : static_cast<std::uint8_t>(
                  snapshot.region >> state::activity::bubble_authority::kSliceSetToBubbleShift);
    const bool hasRetirement = encoded
                               && (placedRetirementPending || (snapshot.hasGrant && enteringBubble))
                               && server::gameplay::squad_entity_retirement::prepare_retirement(
                                   session.activity.session,
                                   session.activity.bindingGeneration,
                                   retirementBubble,
                                   entityRetirement);
    // A retirement-bearing roster must keep its grant and region cursor owed until the
    // keepalive can atomically advance shared State immediately before copying the frame.
    // Sending the roster alone would spend the entering-bubble trigger and strand its purge.
    if (hasRetirement && (!allowEntityRetirement || !epochCurrent)) {
        encoded = false;
    }
    if (placedRetirementPending && allowEntityRetirement && !hasRetirement && !solicited) {
        encoded = false;
    }
    // A private link granted a bubble another link released adopts what that link simulated
    // there, which would otherwise stay frozen as replicas nobody runs.
    HandoverClaim handoverClaim{};
    state::activity::bubble_authority::EntitySlotMask released{};
    if (encoded && snapshot.hasGrant && sharedBubbles
        && session.activity.role == ActivityClientRole::privateCurrent
        && state::activity::bubble_authority::snapshot_released_entities(
            sessionId, grant.bubble, released)) {
        handoverClaim =
            handover_claim({.sharedBubbles = sharedBubbles,
                            .granted = true,
                            .bubble = grant.bubble,
                            .recipient = session.activity.bindingGeneration,
                            .releasedBy = releaser,
                            .released = &released,
                            .purged = hasRetirement ? &entityRetirement.entities : nullptr,
                            .heldEpoch = session.activity.replicationEpoch});
    }
    // An unsolicited body identical to the last delivered one is skipped. A solicited one never
    // is. The repeat check knows only this host's own history, and a slice-set teardown clears
    // the client's mirror without telling us, which is exactly when it asks again.
    const bool suppressible = encoded && !solicited && !snapshot.hasGrant && !ownsHostState
                              && !ownsPending && !missionSeedPending && !placedRetirementPending;
    // Which terms held is in the log line, because a repeat that one of them forced reaches the
    // client as a fresh apply.
    const std::uint8_t forced = static_cast<std::uint8_t>(
        (solicited ? kRosterForceSolicited : 0U) | (snapshot.hasGrant ? kRosterForceGrant : 0U)
        | (ownsHostState ? kRosterForceHostState : 0U) | (ownsPending ? kRosterForceScriptable : 0U)
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
        if (estatePending) {
            // The client holds this exact body already, so it has what the estate owed it.
            session.activityEstateRevision = hostState.estateRevision;
            session.activityDialogueRevision = hostState.dialogueRevision;
        }
        report_roster_push(
            session, snapshot, name, 0, kNoGrant, RosterOutcome::unchanged, bodyHash, forced);
        return false;
    }
    if (encoded && hasRetirement) {
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
        if (encoded) {
            middleware::secure_channel::advance_nonce(nonce);
        }
    }
    if (encoded && entitySlotAllocation.prepared) {
        namespace slotWire = middleware::bap::activity_message::entity_slots;
        // Keep the encoded roster intact: both native notifications share the same ordered batch.
        std::array<std::byte, slotWire::kEncodedSize> allocationBytes{};
        std::size_t allocationSize{};
        encoded = slotWire::encode_entity_slots(
                      entitySlotAllocation.mask, allocationBytes, allocationSize)
                  && append_notification_frame(scratch,
                                               session.activity.session.sessionId,
                                               slotWire::kNotificationMessageType,
                                               std::span(allocationBytes).first(allocationSize),
                                               key,
                                               nonce,
                                               response,
                                               written);
        if (encoded) {
            middleware::secure_channel::advance_nonce(nonce);
        }
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
    // The claim follows the grant it belongs to; the nonce advance below covers the last frame.
    if (encoded && handoverClaim.pending) {
        namespace control = middleware::bap::activity_message::host_control;
        middleware::secure_channel::advance_nonce(nonce);
        const control::ClaimAuthorityBody claim{.slots = handoverClaim.slots,
                                                .epoch = handoverClaim.epoch,
                                                .reason = kClaimAuthorityReason};
        std::array<std::byte, control::kClaimAuthorityByteCount> claimBytes{};
        std::size_t claimSize{};
        encoded = control::encode_claim_authority(claim, claimBytes, claimSize)
                  && append_notification_frame(scratch,
                                               session.activity.session.sessionId,
                                               control::kClaimAuthorityMessageType,
                                               std::span(claimBytes).first(claimSize),
                                               key,
                                               nonce,
                                               response,
                                               written);
    }
    // The frame contains a complete roster, not a delta against this host map. Capture the exact
    // group order only after both encoders accept it. A malformed identity map refuses the whole
    // attempt, and the rollback below removes the staged bytes and counters.
    encoded =
        encoded
        && build_roster_decode_map(snapshot.roster, session.activity.bindingGeneration, decodeMap);
    encoded = encoded && stage_roster_device_publications(session, scratch, snapshot);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        // The deferred answer is discharged by the body that carries it.
        session.activityRosterOwedForEpoch = false;
        // Staged, not published. The grant and the counters are one-way and this body may still be
        // discarded, so they are held here and settled by `commit_staged_roster` or
        // `discard_staged_roster`.
        session.activityRosterStaged.grant = grant;
        session.activityRosterStaged.entitySlotAllocation = entitySlotAllocation;
        session.activityRosterStaged.entityRetirement = entityRetirement;
        session.activityRosterStaged.retirementSequence = retirementSequence;
        session.activityRosterStaged.retirementPriorEpoch = retirementPriorEpoch;
        session.activityRosterStaged.retirementBaseEpoch = retirementBaseEpoch;
        session.activityRosterStaged.retirementEpoch = retirementEpoch;
        session.activityRosterStaged.handoverClaim = handoverClaim;
        session.activityRosterStaged.handedSquads = handedSquads;
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
        // A link whose body has no participation record never held the spawn, so owes no answer.
        session.activityRosterStaged.awaitClientSync =
            snapshot.awaitClientSync && carries_participation(snapshot.roster);
        const MissionSeedLease& missionSeed = session.activityMissionSeed;
        session.activityRosterStaged.missionSeedRevision = missionSeed.revision;
        session.activityRosterStaged.scriptableOverride = scriptablePending;
        session.activityRosterStaged.hasGrant = snapshot.hasGrant;
        session.activityRosterStaged.hasHostState = ownsHostState;
        session.activityRosterStaged.estateRevision = hasHostState ? hostState.estateRevision : 0;
        session.activityRosterStaged.dialogueRevision =
            hasHostState ? hostState.dialogueRevision : 0;
        session.activityRosterStaged.hasEstateRevision = hasHostState && !peerLeave;
        session.activityRosterStaged.peerLeave = peerLeave;
        // A retired body publishes no seed content, so its revision stays owed.
        session.activityRosterStaged.hasMissionSeedRevision =
            !peerLeave
            && mission_seed_publication_pending(missionSeed, session.activity.bindingGeneration);
        session.activityRosterStaged.missionSeedFullSet =
            !peerLeave && missionSeed.fullSetPublished;
        session.activityRosterStaged.hasScriptableOverride = ownsPending;
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
    if (encoded && peerLeave) {
        std::array<char, core::log::kLineCapacity> line{};
        const int length = std::snprintf(line.data(),
                                         line.size(),
                                         "ev=activity stage=peer_leave result=answered groups=%zu",
                                         retiredGroups);
        if (length > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(length)});
        }
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
        if (ownsHostState) {
            server::activity::host::note_auth_attempt(
                session.activity.session,
                session.activity.bindingGeneration,
                hostState.revision,
                hostState.lifetimeState,
                server::activity::host::OutputStatus::frameRefused);
        }
        if (ownsPending) {
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

namespace {

/** A claim names the epoch its client held when it was built, so a later step makes it stale. */
[[nodiscard]] bool staged_claim_current(const Session& session) noexcept {
    const auto& staged = session.activityRosterStaged;
    return !staged.staged || !staged.handoverClaim.pending
           || (staged.bindingGeneration == session.activity.bindingGeneration
               && staged.handoverClaim.heldEpoch == session.activity.replicationEpoch);
}

} // namespace

bool validate_staged_roster(const Session& session) noexcept {
    const auto& staged = session.activityRosterStaged;
    return staged_claim_current(session)
           && (!staged.staged || !staged.entityRetirement.pending
               || (staged.bindingGeneration == session.activity.bindingGeneration
                   && staged.retirementPriorEpoch == session.activity.replicationEpoch
                   && server::gameplay::squad_entity_retirement::validate_retirement(
                       session.activity.session,
                       session.activity.bindingGeneration,
                       staged.entityRetirement)));
}

/** Retained identities cannot change between final validation and the caller copy. */
bool begin_staged_roster_publication(
    const Session& session, server::gameplay::entity_identities::PublicationLease& lease) noexcept {
    const auto& staged = session.activityRosterStaged;
    return staged_claim_current(session)
           && (!staged.staged || !staged.entityRetirement.pending
               || (staged.bindingGeneration == session.activity.bindingGeneration
                   && staged.retirementPriorEpoch == session.activity.replicationEpoch
                   && server::gameplay::squad_entity_retirement::begin_retirement_publication(
                       session.activity.session,
                       session.activity.bindingGeneration,
                       staged.entityRetirement,
                       lease)));
}

namespace {

/** Tells the mission surface that the arrival answer reached the client, which spawns on it. */
void note_client_entered(const Session& session) noexcept {
    server::activity::host::ClientStateChangeInput input{};
    input.binding = session.activity.session;
    input.state.heldRegion =
        state::activity::membership::player_region(session.activity.session.sessionId);
    input.state.activityStateRevision = state::activity::membership::state_revision();
    input.state.entered = true;
    input.state.committed = true;
    input.sourceGeneration = session.activity.bindingGeneration;
    // Only Sense observations read the sequence; the arrival answer has no client message.
    input.clientMessageSequence = input.state.activityStateRevision;
    const bool queued = server::activity::host::submit_client_state_change(input);
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=client_entered result=%s region=%d",
                                      queued ? "ok" : "refused",
                                      input.state.heldRegion);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         queued ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

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
    if (!session.activityRosterStaged.peerLeave) {
        const bool answeredArrival =
            session.activityRosterAwaitClientSync && !session.activityRosterStaged.awaitClientSync;
        session.activityRosterAwaitClientSync = session.activityRosterStaged.awaitClientSync;
        if (answeredArrival) {
            note_client_entered(session);
        }
    }
    if (session.activityRosterStaged.entityRetirement.pending) {
        server::gameplay::squad_entity_retirement::commit_retirement(
            session.activityRosterStaged.entityRetirement);
        const auto& staged = session.activityRosterStaged;
        commit_replication_steps(session, staged.retirementSequence + 1);
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
    if (const auto& claim = session.activityRosterStaged.handoverClaim; claim.pending) {
        // Delivered entities are no longer released, and the previous holder's captured trees
        // for them are this link's live entities now, so its renewal must not purge them.
        state::activity::bubble_authority::record_claim(
            session.activity.session.sessionId, claim.bubble, claim.slots);
        server::gameplay::squad_entity_retirement::returned_slots(
            session.activity.session, claim.releasedBy, claim.slots);
    }
    if (const auto& handed = session.activityRosterStaged.handedSquads; handed.count != 0) {
        // The recipient's next delta is relative to the state it was just handed.
        static_cast<void>(server::activity::host::seed_handover_squad_sense(
            session.activity.session,
            session.activity.bindingGeneration,
            handed.releaser,
            handed.bubble,
            std::span(handed.keys).first(handed.count)));
    }
    if (session.activityRosterStaged.hasGrant) {
        state::activity::bubble_authority::record_grant(session.activity.session.sessionId,
                                                        session.activity.bindingGeneration,
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
    commit_mission_seed_publication(session, session.activityRosterStaged);
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
    if (session.activityRosterStaged.hasEstateRevision) {
        // The body carried the estate as read when it was built. A pending body it also carried
        // joined the estate just above, delivered by this same body, so the cursors take that.
        server::activity::host::AuthState hostState{};
        const bool staged =
            scriptableCommitReady
            && server::activity::host::auth_state(session.activity.session, hostState);
        session.activityEstateRevision =
            staged ? hostState.estateRevision : session.activityRosterStaged.estateRevision;
        session.activityDialogueRevision =
            staged ? hostState.dialogueRevision : session.activityRosterStaged.dialogueRevision;
    }
    commit_roster_device_publications(session);
    if (session.activityRosterStaged.peerLeave) {
        session.activityLeftGeneration = session.activity.bindingGeneration;
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
