#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Appends one encrypted svc9 notification on the supplied local nonce.
 * @param scratch Lock-owned transform buffers whose request plaintext is no longer borrowed.
 * @param sessionId Nonzero activity id echoed in the svc9 envelope.
 * @param messageType Typed activity notification id.
 * @param messageBody Whole typed activity message body.
 * @param key Active AES-GCM session key.
 * @param nonce Current local send nonce, not modified by this function.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing byte count, advanced only after a complete frame exists.
 * @return True when the activity envelope, BAP header, seal, and outer frame all fit.
 */
[[nodiscard]] bool append_notification_frame(Scratch& scratch,
                                             std::uint64_t sessionId,
                                             std::uint32_t messageType,
                                             std::span<const std::byte> messageBody,
                                             std::span<const std::byte, state::kAesKeySize> key,
                                             std::span<const std::byte, state::kBapNonceSize> nonce,
                                             std::span<std::byte> response,
                                             std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
