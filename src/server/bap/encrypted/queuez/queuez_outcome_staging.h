#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "../internal.h"
#include "definition.h"

namespace sunrise::server::bap::encrypted::queuez {

/**
 * Stages any queuez frames and peer after-image that one service outcome asks for.
 * A frame that cannot be built is logged and skipped, never turned into a refused reply.
 * @param scratch Transform buffers owned by the lock.
 * @param before Queuez state the current BAP peer can see.
 * @param outcome Body outcome carrying at most one queuez action.
 * @param key Active AES-GCM session key.
 * @param nonce Local send nonce, advanced only by whole staged frames.
 * @param response Whole-frame staging storage owned by the lock.
 * @param written Bytes already staged, updated only by whole frames.
 * @param publication Gets a queuez after-image when an action records state.
 * @return True when every asked-for frame fits. Unsubscription is idempotent on its own.
 */
[[nodiscard]] bool stage_service_outcome(Scratch& scratch,
                                         const SessionState& before,
                                         const ServiceOutcome& outcome,
                                         std::span<const std::byte, state::kAesKeySize> key,
                                         std::array<std::byte, state::kBapNonceSize>& nonce,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         StagedPublication& publication) noexcept;

} // namespace sunrise::server::bap::encrypted::queuez
