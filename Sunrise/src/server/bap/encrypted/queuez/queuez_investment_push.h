#pragma once

#include <cstddef>
#include <span>

#include "../internal.h"
#include "queuez_item_grant_push.h"

namespace sunrise::server::bap::encrypted {

/**
 * Encodes a refresh of the objects a saved change names, read from State as it stands, a caller's
 * uncommitted savepoint included. That is the selected character, plus the account when its rows
 * changed, and ahead of them the held item whose own values changed.
 * @param changed The objects to refresh; the character must be the selected one.
 * @param frame Receives the frame held in scratch and the peer state that applies once it is sent.
 * @param reason Receives inventory_stage or inventory_encode on failure.
 * @return True when the frame is encoded. Nothing is sent and the session is unchanged.
 */
[[nodiscard]] bool stage_changed_objects(const state::AccountEditChange& changed,
                                         Session& session,
                                         Scratch& scratch,
                                         StagedFrame& frame,
                                         const char*& reason) noexcept;

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
