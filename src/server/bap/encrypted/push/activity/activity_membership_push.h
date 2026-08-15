#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../../../internal.h"
#include "../../activity_message/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Appends one current membership svc9 notification and advances its local nonce.
 * @param scratch Lock-owned transform buffers.
 * @param activity Session and prepared State snapshot picked by the svc8 route.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @return True when the snapshot and notification encode atomically.
 */
[[nodiscard]] bool
append_membership_notification(Scratch& scratch,
                               const activity_message::ActivityPlan& activity,
                               std::span<const std::byte, state::kAesKeySize> key,
                               std::array<std::byte, state::kBapNonceSize>& nonce,
                               std::span<std::byte> response,
                               std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
