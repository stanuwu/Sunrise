#include "activity_roster_push.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/activity/bubble_authority/runtime.h"
#include "activity_notification_frame.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace message = middleware::bap::activity_message::sensor_auth_update;

/** No bubble was granted with this body. */
constexpr std::int32_t kNoGrant = -1;
/** The destination name a refusal reports. The selection field is 40 bytes wide. */
constexpr std::size_t kDestinationCapacity = 40;

} // namespace

/** Appends one `sensor_auth_update` svc9 notification carrying the destination's roster. */
bool append_roster_notification(Session& session,
                                Scratch& scratch,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool burst) noexcept {
    if (written > response.size()) {
        return false;
    }
    const std::uint32_t initialRosterGroups = session.activityRosterGroups;
    const std::uint8_t initialRosterSends = session.activityRosterSends;
    const std::uint8_t initialRosterState = session.activityRosterState;
    message::Snapshot snapshot{};
    std::array<char, kDestinationCapacity> destination{};
    std::size_t destinationLength = 0;
    RosterOutcome outcome = RosterOutcome::noEpoch;
    if (session.activityPatchEpochSeen) {
        outcome = build_roster_snapshot(
            session, scratch, snapshot, destination, destinationLength, burst);
    }
    const std::string_view name(destination.data(), destinationLength);
    if (outcome != RosterOutcome::published) {
        report_roster_push(session, snapshot, name, 0, kNoGrant, outcome);
        return false;
    }

    // The grant is committed only after the body carrying it is staged, so a failed stage leaves
    // the bubble ungranted and the next push retries it.
    state::activity::bubble_authority::Grant grant{};
    // The grant follows the player, not the destination. The client names the region it is in
    // and that moves as it walks between bubbles, so granting the arrival bubble again would leave
    // the bubble the player actually entered without authority.
    // The wire field is unsigned. A value past the signed range turns negative and the selector
    // rejects it, the same answer as its own upper bound.
    if (state::activity::bubble_authority::select_grant(
            session.activitySessionId, static_cast<std::int32_t>(snapshot.region), grant)) {
        snapshot.hasGrant = true;
        snapshot.grant.bubble = grant.bubble;
        snapshot.grant.token = grant.token;
    }

    bool recorded = false;
    const std::size_t initialWritten = written;
    auto initialNonce = nonce;
    std::size_t messageSize = 0;
    bool encoded = message::encode_sensor_auth_update(snapshot, scratch.responseBody, messageSize)
                   && append_notification_frame(scratch,
                                                session.activitySessionId,
                                                message::kMessageType,
                                                std::span(scratch.responseBody).first(messageSize),
                                                key,
                                                nonce,
                                                response,
                                                written);
    if (encoded) {
        middleware::secure_channel::advance_nonce(nonce);
        // The grant is recorded only after the body carrying it is staged, so a failed stage
        // leaves the bubble ungranted and the next push retries it.
        if (snapshot.hasGrant) {
            state::activity::bubble_authority::record_grant(session.activitySessionId, grant);
            recorded = true;
        }
    }
    report_roster_push(session,
                       snapshot,
                       name,
                       encoded ? messageSize : 0,
                       recorded ? snapshot.grant.bubble : kNoGrant,
                       encoded ? RosterOutcome::published : RosterOutcome::encodeFailed);
    SecureZeroMemory(scratch.responseBody.data(), messageSize);
    if (!encoded) {
        if (written > initialWritten) {
            SecureZeroMemory(response.data() + initialWritten, written - initialWritten);
        }
        written = initialWritten;
        nonce = initialNonce;
        session.activityRosterGroups = initialRosterGroups;
        session.activityRosterSends = initialRosterSends;
        session.activityRosterState = initialRosterState;
    }
    SecureZeroMemory(&initialNonce, sizeof initialNonce);
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::push::activity
