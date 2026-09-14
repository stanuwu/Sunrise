#pragma once

#include "../../../../../middleware/bap/activity_message/definition.h"
#include "../../../../../state/activity/reservations/definition.h"
#include "../definition.h"

namespace sunrise::server::bap::encrypted::activity_message::membership {
/** One retract the host declined to act on, named for the caller's report. */
struct ReleaseRefusalReport final {
    std::uint64_t peerKey{};
    state::activity::reservations::ReleaseRefusal refusal{};
};
[[nodiscard]] bool prepare_reservations(const middleware::bap::activity_message::Request& request,
                                        ActivityPlan& plan) noexcept;
/**
 * @param refusal Filled when the retract named a member the host declines to unseat -- its own
 *        primary row or a row still holding entity slots. That is an accepted no-op, not a
 *        malformed body, and the caller frames and reports it as one.
 */
[[nodiscard]] bool
prepare_reservation_release(const middleware::bap::activity_message::Request& request,
                            ActivityPlan& plan,
                            ReleaseRefusalReport& refusal) noexcept;
} // namespace sunrise::server::bap::encrypted::activity_message::membership
