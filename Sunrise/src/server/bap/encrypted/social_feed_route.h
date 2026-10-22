#pragma once
#include "../../../middleware/bap/frame.h"

namespace sunrise::server::bap {
struct Session;
struct Scratch;
namespace encrypted {
/** Caller owns the BAP lock. Change-driven route refresh continues without a local BAP link;
 * the host's social mirror is applied only while locally connected. */
void service_host_social() noexcept;
/** Drops the host mirror's published stamp when the shared directory itself is reconstructed. */
void reset_host_social() noexcept;
/**
 * Applies a client's social sync request to the shared directory and answers with the
 * resulting feed. False leaves the shared directory untouched.
 */
[[nodiscard]] bool consume_social_feed(Session& session,
                                       Scratch& scratch,
                                       const middleware::bap::RequestFrame& request,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept;
/**
 * Publishes one social publication notice to the connection that registered for this account.
 * @param touchesScratch Set when the attempt reaches a scratch buffer.
 * @return True when a notification is published; a refusal leaves the stamp and nonce unspent.
 */
[[nodiscard]] bool consume_social_notice(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept;
} // namespace encrypted
} // namespace sunrise::server::bap
