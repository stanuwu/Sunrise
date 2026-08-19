#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../../middleware/bap/activity_message/activity_global_state_encoder.h"
#include "../../../../../state/activity/definition.h"
#include "../../../../../state/activity/destination/definition.h"
#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Builds the whole message body input for one session.
 * @param binding Exact activity generation whose destination is encoded.
 * @param output Cleared, then receives the destination found and the bubble policy.
 * @param selection Caller storage the body's descriptor view points into.
 * @return True when the authored defaults are present.
 */
[[nodiscard]] bool
resolve_state(const state::activity::SessionBinding& binding,
              middleware::bap::activity_message::global_activity_state::GlobalActivityState& output,
              state::activity::destination::DestinationSelection& selection) noexcept;

/**
 * Appends one global-activity-state svc9 notification and advances its local nonce.
 * The body is built from the destination committed with the session, falling back to the authored
 * default destination and its bubble policy when no scenario table is present.
 * @param scratch Lock-owned transform buffers.
 * @param binding Exact activity generation echoed in the svc9 envelope.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @return True when the state is built and the notification encodes atomically.
 */
[[nodiscard]] bool
append_global_state_notification(Scratch& scratch,
                                 const state::activity::SessionBinding& binding,
                                 std::span<const std::byte, state::kAesKeySize> key,
                                 std::array<std::byte, state::kBapNonceSize>& nonce,
                                 std::span<std::byte> response,
                                 std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
