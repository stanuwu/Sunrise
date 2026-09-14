#pragma once
#include "../../../middleware/bap/frame.h"

namespace sunrise::server::bap {
struct Session;
struct Scratch;
namespace encrypted {
/** Caller owns the BAP lock, as for a remote social-feed request. */
void service_host_social(std::uint64_t now) noexcept;
[[nodiscard]] bool consume_social_feed(Session& session,
                                       Scratch& scratch,
                                       const middleware::bap::RequestFrame& request,
                                       std::span<std::byte> response,
                                       std::size_t& written) noexcept;
} // namespace encrypted
} // namespace sunrise::server::bap
