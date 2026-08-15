#pragma once

#include <cstddef>
#include <span>

#include "../../../internal.h"
#include "../../activity_message/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Appends one `sensor_auth_update` svc9 notification carrying the destination's roster.
 * Nothing is staged before the client's own patch epoch has arrived, because a body carrying a
 * wrong epoch still lands phase 1, skips phase 2, and reports nothing.
 * @param session Connection-owned nonce, activity binding, epoch, and roster counters.
 * @param scratch Lock-owned transform and roster buffers.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only after the complete notification exists.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only after the notification exists.
 * @param burst True for a send on the loading cadence, which must not move the state byte.
 * @return True when the roster is built and the type-5 frame encodes atomically.
 */
[[nodiscard]] bool append_roster_notification(Session& session,
                                              Scratch& scratch,
                                              std::span<const std::byte, state::kAesKeySize> key,
                                              std::array<std::byte, state::kBapNonceSize>& nonce,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              bool burst) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
