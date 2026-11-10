#pragma once
#include "../../../middleware/bap/frame.h"

namespace sunrise::server::bap {
struct Session;
struct Scratch;
namespace encrypted {
/** Applies a validated public projection before acknowledging it on the owning connection. */
[[nodiscard]] bool consume_account_projection(Session& session,
                                              Scratch& scratch,
                                              const middleware::bap::RequestFrame& request,
                                              std::span<std::byte> response,
                                              std::size_t& written) noexcept;
} // namespace encrypted
} // namespace sunrise::server::bap
