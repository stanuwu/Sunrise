#pragma once

#include <cstddef>
#include <span>

#include "../internal.h"

namespace sunrise::server::bap::encrypted {

/**
 * Runs the oldest editor operation submitted against this peer's account and records its receipt.
 * An operation commits only together with its frame. The one exception is a grant whose frame
 * failed after State committed it, after which every Family-4 peer resyncs the account.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a frame was written because the operation, or one step of a grant, published.
 */
[[nodiscard]] bool consume_investment_operation(Session& session,
                                                Scratch& scratch,
                                                std::span<std::byte> response,
                                                std::size_t& written,
                                                bool& touchesScratch) noexcept;

} // namespace sunrise::server::bap::encrypted
