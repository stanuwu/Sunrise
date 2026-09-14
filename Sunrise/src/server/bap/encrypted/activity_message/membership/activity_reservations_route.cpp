#include "activity_reservations_route.h"

#include <array>

#include "../../../../../middleware/bap/activity_message/peer_ledger.h"
#include "../../../../../middleware/bap/activity_message/telemetry.h"
#include "../../../../../state/activity/reservations/runtime.h"
#include "../native_member_identity.h"

namespace sunrise::server::bap::encrypted::activity_message::membership {
namespace service = middleware::bap::activity_message;

bool prepare_reservations(const service::Request& request, ActivityPlan& plan) noexcept {
    service::telemetry::ReservationRequest parsed{};
    std::size_t consumed{};
    if (!service::telemetry::parse_reservation_request(request.payload, parsed, consumed)) {
        return false;
    }
    std::array<state::activity::membership::Identity, service::telemetry::kPeerRecordCapacity>
        identities{};
    for (std::size_t i = 0; i < parsed.recordCount; ++i) {
        identities[i] = native_member_identity(parsed.records[i]);
    }
    if (!state::activity::reservations::prepare_admit(
            request.sessionId,
            std::span(identities).first(parsed.recordCount),
            plan.reservationMutation)
        || parsed.peerTableEpoch != plan.reservationMutation.peerTableEpoch) {
        plan.reservationMutation = {};
        return false;
    }
    plan.sessionId = request.sessionId;
    plan.mutationDomain = MutationDomain::reservations;
    // The committed membership revision remains owed by the existing keepalive until delivered.
    plan.delivery = Delivery::none;
    return true;
}

bool prepare_reservation_release(const service::Request& request,
                                 ActivityPlan& plan,
                                 ReleaseRefusalReport& refusal) noexcept {
    refusal = {};
    service::peer_ledger::ReservationRelease release{};
    std::size_t consumed{};
    if (!service::peer_ledger::parse_release(request.payload, release, consumed)) {
        return false;
    }
    auto outcome = state::activity::reservations::ReleaseRefusal::none;
    if (!state::activity::reservations::prepare_release(
            request.sessionId, release.peerKey, plan.reservationMutation, &outcome)) {
        refusal = {release.peerKey, outcome};
        return false;
    }
    plan.sessionId = request.sessionId;
    plan.mutationDomain = MutationDomain::reservations;
    plan.delivery = Delivery::none;
    return true;
}
} // namespace sunrise::server::bap::encrypted::activity_message::membership
