#pragma once

#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
/** Retains the notification and nonce until a complete encrypted frame reaches the caller. */
[[nodiscard]] bool consume_relay_notifications(Session& session,
                                               Scratch& scratch,
                                               std::span<std::byte> response,
                                               std::size_t& written,
                                               bool& touchesScratch) noexcept;
} // namespace sunrise::server::bap::encrypted::push::activity
