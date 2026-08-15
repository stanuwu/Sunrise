#pragma once

#include "../../../../../middleware/bap/activity_message/definition.h"
#include "../definition.h"

namespace sunrise::server::bap::encrypted::activity_message::membership {

/**
 * Stages a changed identity push or an unchanged transactional no-op.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the membership transaction and optional delivery.
 * @return True when the exact identity is valid for the join-bound session.
 */
[[nodiscard]] bool prepare_identity(const middleware::bap::activity_message::Request& request,
                                    ActivityPlan& plan) noexcept;

/**
 * Stages the kept host-state changes, and an updated snapshot only when needed.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the authoritative membership transaction.
 * @return True when the exact sparse body is valid for the joined session.
 */
[[nodiscard]] bool prepare_authoritative(const middleware::bap::activity_message::Request& request,
                                         ActivityPlan& plan) noexcept;

/**
 * Stages a stable membership resend guarded by the current State revisions.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the refresh transaction and optional delivery.
 * @return True when the exact refresh request and joined session are valid.
 */
[[nodiscard]] bool prepare_refresh(const middleware::bap::activity_message::Request& request,
                                   ActivityPlan& plan) noexcept;

/**
 * Stages a matching acknowledgement update or a transactional no-op.
 * @param request Validated owned svc8 envelope.
 * @param plan Cleared, then receives the acknowledgement transaction.
 * @return True when the exact acknowledgement and joined session are valid.
 */
[[nodiscard]] bool
prepare_acknowledgement(const middleware::bap::activity_message::Request& request,
                        ActivityPlan& plan) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message::membership
