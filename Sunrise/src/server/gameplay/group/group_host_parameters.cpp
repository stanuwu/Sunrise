#include "group_host_parameters.h"

#include <array>

#include "../../../core/settings/settings.h"
#include "../../../middleware/gameplay/group/current_activity_body.h"
#include "../../../middleware/gameplay/group/parameter_messages.h"
#include "../../../middleware/gameplay/group/parameter_registry.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "group_host_internal.h"
#include "group_host_sessions.h"

namespace sunrise::server::gameplay::group {

namespace {

namespace wire = middleware::gameplay::group;
namespace bits = middleware::encoding::bits;

/** Players a session holds. This is the client's own fallback when no activity names a capacity. */
constexpr std::uint8_t kSessionPlayerCapacity = 12;
/** No join-policy bit is set. Any set bit disables the peer's user-join lane. */
constexpr std::uint8_t kOpenJoinPolicyFlags = 0;
/** Join queue mode `none`. This host queues no join, it admits or refuses one. */
constexpr std::uint8_t kJoinQueueModeNone = 0;
/** Every session this host advertises names a host member, which the snapshot's tag 2 selects. */
constexpr bool kHostMemberSelected = true;

/** @return The bit one parameter occupies in a carried, released or requested mask. */
[[nodiscard]] constexpr std::uint64_t parameter_mask(wire::Parameter parameter) noexcept {
    return std::uint64_t{1} << static_cast<std::uint8_t>(parameter);
}

/** The parameter bits this host carries in every group snapshot it publishes. */
constexpr std::uint64_t kHostSelectedMask = parameter_mask(wire::Parameter::hostSelected);
constexpr std::uint64_t kActivityHostMask = parameter_mask(wire::Parameter::activityHost);
constexpr std::uint64_t kCurrentActivityMask = parameter_mask(wire::Parameter::currentActivity);
constexpr std::uint64_t kPreviousActivityMask = parameter_mask(wire::Parameter::previousActivity);
constexpr std::uint64_t kActiveJoinControlsMask =
    parameter_mask(wire::Parameter::activeJoinControls);

/** Resolves one session's exact descriptor nonce, falling back only when none was captured. */
[[nodiscard]] bool selection_nonce(const state::activity::SessionBinding& session,
                                   std::uint64_t& output) noexcept {
    const auto& destination = session.destination;
    output = destination.hasSelectionNonce ? destination.selectionNonce : session.sessionId;
    return wire::current_activity::nonce_is_valid(output);
}

/** Fills `activity-host` from one exact host binding and the shared selection nonce. */
void fill_activity_host(wire::ActivityHostParameter& body,
                        const HostSessionBinding& binding,
                        std::uint64_t selectionNonce,
                        std::uint32_t memberMask) noexcept {
    // The peer builds no join request unless this matches the `current-activity` nonce. The
    // fireteam's rejoin blocker also refuses a zero nonce.
    body.selectionId = selectionNonce;
    // The peer addresses its activity join request to this id. The activity route refuses one that
    // names no committed activity session, and a gameplay identity is not one.
    body.hostId = binding.target.sessionId;
    // The peer tests the bit of its own member index, which the snapshot assigned.
    body.memberMask = memberMask;
    body.address = endpoint::advertised().address;
    body.port = core::settings::get().server.bapPort;
}

/** Fills `current-activity` from the same binding and nonce as `activity-host`. */
void fill_current_activity(wire::ParameterUpdate& update,
                           const HostSessionBinding& binding,
                           std::uint64_t selectionNonce) noexcept {
    const auto& destination = binding.target.destination;
    update.currentActivityReason = destination.reason;
    update.currentActualActivityIndex = destination.sourceActivityIndex;
    update.currentActivityIndex = destination.activityIndex;
    update.currentActivityNonce = selectionNonce;
}

/**
 * Fills `active-join-controls` from the session capacity and the players already in it.
 * The peer derives no part of this body itself: the authority publishes all nine fields.
 * @param body Receives the join policy.
 * @param playerCount Players the session holds now.
 */
void fill_active_join_controls(wire::ActiveJoinControlsParameter& body,
                               std::uint8_t playerCount) noexcept {
    const std::uint8_t free = playerCount < kSessionPlayerCapacity
                                  ? static_cast<std::uint8_t>(kSessionPlayerCapacity - playerCount)
                                  : 0;
    body.totalPlayerCapacity = kSessionPlayerCapacity;
    body.userJoinSlots = free;
    body.partyJoinSlots = free;
    body.remainingPlayerCapacity = free;
    // Each lane is open exactly while a slot is left. The policy flags below are clear, which is
    // the other condition the peer tests before it enables the user-join lane.
    body.userJoinEnabled = free != 0;
    body.partyJoinEnabled = free != 0;
    body.remainingJoinEnabled = free != 0;
    body.joinPolicyFlags = kOpenJoinPolicyFlags;
    body.joinQueueMode = kJoinQueueModeNone;
}

/** Fills `previous-activity` from the target the row's last claim replaced, when it had one. */
void fill_previous_activity(wire::ParameterUpdate& update,
                            const HostSessionBinding& binding) noexcept {
    std::uint64_t nonce = 0;
    if (binding.previous.sessionId == state::activity::kAbsentSessionId
        || !selection_nonce(binding.previous, nonce)) {
        return;
    }
    const auto& destination = binding.previous.destination;
    update.previousActivityReason = destination.reason;
    update.previousActualActivityIndex = destination.sourceActivityIndex;
    update.previousActivityIndex = destination.activityIndex;
    update.previousActivityNonce = nonce;
}

} // namespace

bool send_parameter_update(const wire::ParameterUpdate& update,
                           const state::gameplay::Endpoint& endpoint) noexcept {
    return send_reliable(
        endpoint,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
}

/** Publishes the `activity-host` parameter for one admitted peer. */
bool publish_activity_host(const state::gameplay::Endpoint& endpoint,
                           std::uint64_t sessionId,
                           std::uint32_t memberMask) noexcept {
    // The body is built from this copy, so no retain is needed: `host_session_for_group` returns
    // only a ready row whose State bindings still match, and nothing below reads the table.
    HostSessionBinding binding{};
    if (!host_session_for_group(sessionId, binding)) {
        // Publishing a zero host id latches an unusable parameter on the peer, and the peer only
        // reads it once. The caller keeps the parameter owed until a row is ready.
        report(core::log::Level::debug, "ev=gameplay stage=activityhost result=nosession");
        return false;
    }
    std::uint64_t selectionNonce = 0;
    if (!selection_nonce(binding.target, selectionNonce)) {
        // A present descriptor nonce is exact. Publishing a different fallback would split the
        // activity-host identity from the selection the peer already owns.
        report(core::log::Level::warn, "ev=gameplay stage=activityhost result=invalidnonce");
        return false;
    }
    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    // All three go in one update, so the peer never holds the host without the activity it
    // belongs to. The replaced descriptor moves to `previous-activity` in the same step.
    update.carriedMask = kActivityHostMask | kCurrentActivityMask | kPreviousActivityMask;
    fill_activity_host(update.activityHost, binding, selectionNonce, memberMask);
    fill_current_activity(update, binding, selectionNonce);
    fill_previous_activity(update, binding);

    const bool sent = send_parameter_update(update, endpoint);
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=activityhost result=%s host=0x%llX address=0x%08X port=%u names=%s",
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(update.activityHost.hostId),
           update.activityHost.address,
           static_cast<unsigned>(update.activityHost.port),
           wire::parameter_names(update.carriedMask, names.data(), names.size()));
    return sent;
}

void answer_parameters(const state::gameplay::Endpoint& endpoint,
                       std::uint64_t sessionId,
                       std::uint64_t requested,
                       std::uint8_t playerCount,
                       std::uint32_t memberMask) noexcept {
    const std::uint64_t selected = requested & wire::kParameterMaskBits;
    std::uint64_t carried = selected & wire::kEncodableParameters;
    // Releasing an empty slot is a no-op on the peer, so a parameter with no encoder here is safe
    // to name. A parameter that has an encoder but no ready binding stays owed instead: releasing
    // it would drop the copy the peer already applied.
    const std::uint64_t released = selected & ~wire::kEncodableParameters;
    // The body is built from this copy, so no retain is needed. See publish_activity_host.
    HostSessionBinding binding{};
    const bool needsActivity =
        (carried & (kActivityHostMask | kCurrentActivityMask | kPreviousActivityMask)) != 0;
    const bool hasActivityBinding = needsActivity && host_session_for_group(sessionId, binding);
    std::uint64_t selectionNonce = 0;
    const bool hasActivity = hasActivityBinding && selection_nonce(binding.target, selectionNonce);
    if (hasActivityBinding && !hasActivity) {
        report(core::log::Level::warn, "ev=gameplay stage=parameters result=invalidnonce");
    }
    if (!hasActivity) {
        // A zero host id is worse than no answer for this one.
        carried &= ~kActivityHostMask;
    }
    if (carried == 0 && released == 0) {
        // Every requested parameter has an encoder and none has a ready binding yet. The caller
        // keeps it owed and republishes it, so this request is answered by that publish.
        report(core::log::Level::debug,
               "ev=gameplay stage=parameters result=deferred mask=0x%08X",
               static_cast<unsigned>(selected));
        return;
    }

    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.carriedMask = carried;
    update.releasedMask = released;
    // A zero host id latches an unusable parameter on the peer, so the answer carries the same
    // body the unsolicited publish does.
    if (hasActivity && (carried & kActivityHostMask) != 0) {
        fill_activity_host(update.activityHost, binding, selectionNonce, memberMask);
    }
    if (hasActivity && (carried & kCurrentActivityMask) != 0) {
        fill_current_activity(update, binding, selectionNonce);
    }
    if (hasActivityBinding && (carried & kPreviousActivityMask) != 0) {
        fill_previous_activity(update, binding);
    }
    if ((carried & kActiveJoinControlsMask) != 0) {
        fill_active_join_controls(update.activeJoinControls, playerCount);
    }
    if ((carried & kHostSelectedMask) != 0) {
        update.hostSelected = kHostMemberSelected;
    }

    const bool sent = send_parameter_update(update, endpoint);
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s carried=0x%08X released=0x%08X names=%s",
           sent ? "answered" : "fail",
           static_cast<unsigned>(carried),
           static_cast<unsigned>(released),
           wire::parameter_names(carried, names.data(), names.size()));
}

} // namespace sunrise::server::gameplay::group
