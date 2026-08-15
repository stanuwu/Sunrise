#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../activity_message/definition.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::activity_transaction {

/**
 * Stages the notifications one activity transaction requests.
 * @param session Connection-owned roster counters, advanced only by a staged roster.
 * @param scratch Lock-owned transform buffers.
 * @param activity Prepared activity transaction and delivery selection.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce advanced only by complete staged notifications.
 * @param response Lock-owned complete-frame staging storage.
 * @param written Existing staged byte count, updated only by complete notifications.
 * @return True when every requested notification is staged.
 */
[[nodiscard]] bool stage_notifications(Session& session,
                                       Scratch& scratch,
                                       const activity_message::ActivityPlan& activity,
                                       std::span<const std::byte, state::kAesKeySize> key,
                                       std::array<std::byte, state::kBapNonceSize>& nonce,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_transaction
