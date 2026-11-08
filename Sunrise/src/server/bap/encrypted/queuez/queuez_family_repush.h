#pragma once

#include <cstddef>
#include <span>

#include "../internal.h"

// Re-pushes that rebuild one family's subscribe answer against current State, for a root the peer
// already holds.

namespace sunrise::server::bap::encrypted {

/**
 * Sends the Family-4 snapshot an owed re-push names. The one attempt is spent either way.
 * @param session Auth, nonce, queuez state and the re-push arm owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when the snapshot frame is published.
 */
[[nodiscard]] bool consume_family4_repush(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept;

/**
 * Sends the family-two re-push the change that moved the member record owes.
 * @param session Auth, nonce, queuez state and the re-push arm owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole family-two notification is published.
 */
[[nodiscard]] bool consume_social_roster_repush(Session& session,
                                                Scratch& scratch,
                                                std::span<std::byte> response,
                                                std::size_t& written,
                                                bool& touchesScratch) noexcept;

/**
 * Sends the banner pair the launch cinematic hold armed, once the account names a character.
 * @param session Auth, nonce, queuez state and the re-push arm owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when the banner pair is published.
 */
[[nodiscard]] bool consume_banner_repush(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept;

} // namespace sunrise::server::bap::encrypted
