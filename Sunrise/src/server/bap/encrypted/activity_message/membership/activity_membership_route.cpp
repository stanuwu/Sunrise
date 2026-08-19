#include "activity_membership_route.h"

#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/\
activity_membership_acknowledgement_parser.h"
#include "../../../../../middleware/bap/activity_message/activity_client_identity_parser.h"
#include "../../../../../middleware/bap/activity_message/activity_state_refresh_parser.h"
#include "../../../../../middleware/bap/activity_message/client_authoritative_data.h"
#include "../../../../../middleware/bap/activity_message/start_activity.h"
#include "../../../../../state/activity/destination/definition.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"

namespace sunrise::server::bap::encrypted::activity_message::membership {
namespace {

namespace service = middleware::bap::activity_message;
namespace membership_state = state::activity::membership;

/** A request with no revision of its own asks for the current one, not a numbered resend. */
constexpr std::uint32_t kNoRequestedRevision = 0;

/**
 * Maps one parsed client identity into State's protocol-neutral storage.
 * @param parsed Typed Middleware identity whose source bytes expire after routing.
 * @return Whole State identity, with no borrowed payload views.
 */
[[nodiscard]] membership_state::Identity
make_identity(const service::client_identity::ClientIdentity& parsed) noexcept {
    membership_state::Identity identity{};
    identity.memberKey = parsed.memberKey;
    identity.smallOpaque = parsed.field1;
    identity.signedOpaque = parsed.field2;
    identity.joinIdentity = parsed.field3;
    identity.accountSoid = parsed.accountSoid;
    identity.opaqueSoid = parsed.field5;
    identity.secondaryOpaque = parsed.field6;
    return identity;
}

/**
 * Maps the kept Middleware fields into protocol-neutral sparse State values.
 * @param parsed Typed delta whose source bytes expire after routing.
 * @return Sparse State update with the same kept-field presence.
 */
[[nodiscard]] membership_state::AuthoritativeUpdate make_authoritative(
    const service::client_authoritative_data::ClientAuthoritativeData& parsed) noexcept {
    membership_state::AuthoritativeUpdate update{};
    update.transitionToken = parsed.transitionToken;
    update.hasTransitionToken = parsed.hasTransitionToken;
    update.spawn.state = parsed.spawn.state;
    update.spawn.opaqueByte = parsed.spawn.opaqueByte;
    update.spawn.opaqueValue = parsed.spawn.opaqueValue;
    update.hasSpawn = parsed.hasSpawn;
    update.teleport.state = parsed.teleport.state;
    update.teleport.token = parsed.teleport.token;
    update.teleport.sliceSetIndex = parsed.teleport.sliceSetIndex;
    update.teleport.sliceSetHash = parsed.teleport.sliceSetHash;
    update.hasTeleport = parsed.hasTeleport;
    // The region is what names the player's bubble. Dropping it here leaves the host on the
    // destination's arrival slice set for the whole run, and no bubble crossing grants authority.
    update.region.index = parsed.region.index;
    update.region.hash = parsed.region.hash;
    update.hasRegion = parsed.hasRegion;
    return update;
}

/**
 * Reports one client identity update.
 * Without it, a run where the client sent its own identity looks the same as one where message 12
 * shipped the seeded fallback all the way.
 * @param parsed Typed identity the client sent.
 */
void report_identity(const service::client_identity::ClientIdentity& parsed) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=identity result=ok key=0x%llX field1=%d field2=%d "
                      "field3=0x%llX acct=0x%llX character=0x%llX field6=0x%llX",
                      static_cast<unsigned long long>(parsed.memberKey),
                      static_cast<int>(parsed.field1),
                      static_cast<int>(parsed.field2),
                      static_cast<unsigned long long>(parsed.field3),
                      static_cast<unsigned long long>(parsed.accountSoid),
                      static_cast<unsigned long long>(parsed.field5),
                      static_cast<unsigned long long>(parsed.field6));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Stages a changed identity push or an unchanged transactional no-op. */
bool prepare_identity(const service::Request& request, ActivityPlan& plan) noexcept {
    service::client_identity::ClientIdentity parsed{};
    if (!service::client_identity::parse_client_identity(request.payload, parsed)) {
        return false;
    }
    report_identity(parsed);
    if (!membership_state::prepare_identity(
            request.accountHandle, make_identity(parsed), plan.membershipMutation)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=activity stage=identity result=refused");
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery =
        plan.membershipMutation.changesState ? Delivery::membershipNotification : Delivery::none;
    plan.mutationDomain = MutationDomain::membership;
    return true;
}

/** Stages the kept host-state changes, and an updated snapshot only when needed. */
bool prepare_authoritative(const service::Request& request, ActivityPlan& plan) noexcept {
    service::client_authoritative_data::ClientAuthoritativeData parsed{};
    if (!service::client_authoritative_data::parse_client_authoritative_data(request.payload,
                                                                             parsed)
        || !membership_state::prepare_authoritative(
            request.accountHandle, make_authoritative(parsed), plan.membershipMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.regionMoved = plan.membershipMutation.movesRegion;
    plan.transitionStarted = plan.membershipMutation.movesTransitionToken;
    // A region move sends the roster even when the host state did not change, because the bubble
    // the player just entered has no authority until the roster grants it.
    plan.delivery = plan.membershipMutation.hasSnapshot || plan.regionMoved
                        ? Delivery::authoritativeNotifications
                        : Delivery::none;
    plan.mutationDomain = MutationDomain::membership;
    return true;
}

/** Stages a stable membership resend guarded by the current State revisions. */
bool prepare_refresh(const service::Request& request, ActivityPlan& plan) noexcept {
    service::state_refresh::StateRefresh parsed{};
    if (!service::state_refresh::parse_state_refresh(request.payload, parsed)
        || !membership_state::prepare_refresh(request.accountHandle,
                                              parsed.membershipRevision,
                                              parsed.bubbleIndex,
                                              plan.membershipMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    // A refresh asks for the whole host snapshot. Answering with membership alone leaves the
    // client's own request half answered, and the global state and roster are what it re-reads.
    plan.delivery = Delivery::refreshNotifications;
    plan.mutationDomain = MutationDomain::membership;
    return true;
}

/** Stages the host snapshot a start-new-activity request asks for. */
bool prepare_start_activity(const service::Request& request, ActivityPlan& plan) noexcept {
    service::start_activity::StartActivity parsed{};
    std::size_t consumed = 0;
    if (!service::start_activity::parse_start_activity(request.payload, parsed, consumed)) {
        return false;
    }
    // The destination index is checked against the width its own field can carry. Which indices
    // are installed comes from the activity definition table, which this host does not read, so
    // an in-range index is as far as the route can be validated here.
    const bool routed =
        parsed.destinationActivityIndex >= 0
        && parsed.destinationActivityIndex <= state::activity::destination::kMaximumActivityIndex;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=start_activity result=%s from=%d to=%d "
                                      "tail=%u",
                                      routed ? "accepted" : "out_of_range",
                                      parsed.sourceActivityIndex,
                                      parsed.destinationActivityIndex,
                                      parsed.tailBits);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         routed ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    if (!routed) {
        return false;
    }
    // The request carries no revision or bubble of its own, so the refresh guard is built from the
    // absent pair rather than from a value the client did not send.
    if (!membership_state::prepare_refresh(request.accountHandle,
                                           kNoRequestedRevision,
                                           state::activity::destination::kAbsentActivityIndex,
                                           plan.membershipMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery = Delivery::refreshNotifications;
    plan.mutationDomain = MutationDomain::membership;
    return true;
}

/** Stages a matching acknowledgement update or a transactional no-op. */
bool prepare_acknowledgement(const service::Request& request, ActivityPlan& plan) noexcept {
    service::membership_acknowledgement::MembershipAcknowledgement parsed{};
    if (!service::membership_acknowledgement::parse_membership_acknowledgement(request.payload,
                                                                               parsed)
        || !membership_state::prepare_acknowledgement(
            request.accountHandle, parsed.membershipRevision, plan.membershipMutation)) {
        return false;
    }
    plan.sessionId = request.accountHandle;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::membership;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message::membership
