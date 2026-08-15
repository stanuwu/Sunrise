#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../../../internal.h"
#include "../../activity_message/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Appends the ordered join-result and entity-slot svc9 notifications.
 * @param scratch Lock-owned transform buffers.
 * @param activity Join scalars and the exact lease mask State picked.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced once per staged notification.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after both notifications exist.
 * @return True when both notifications encode atomically.
 */
[[nodiscard]] bool append_join_notifications(Scratch& scratch,
                                             const activity_message::ActivityPlan& activity,
                                             std::span<const std::byte, state::kAesKeySize> key,
                                             std::array<std::byte, state::kBapNonceSize>& nonce,
                                             std::span<std::byte> response,
                                             std::size_t& written) noexcept;

/**
 * Appends one entity-slot svc9 notification and advances its local nonce once.
 * @param scratch Lock-owned transform buffers.
 * @param sessionId Nonzero activity id echoed in the svc9 envelope.
 * @param entitySlots Exact byte-indexed lease mask State picked.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @return True when the notification encodes atomically.
 */
[[nodiscard]] bool
append_entity_slot_notification(Scratch& scratch,
                                std::uint64_t sessionId,
                                std::span<const std::byte> entitySlots,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
