#pragma once

#include <cstdint>

#include "../../middleware/bap/activity_message/transport_report.h"

namespace sunrise::server::bap {
struct Session;

/** Caller holds the BAP lock; only a committed report for the current join may be retained. */
void publish_activity_transport(
    Session& session,
    std::uint64_t bindingGeneration,
    const middleware::bap::activity_message::TransportReport& report) noexcept;
/** Withdraws only this connection's report when its activity binding ends or restarts. */
void clear_activity_transport(Session& session) noexcept;
/** Resolves one character's own native carrier; conflicting live connections are refused. */
[[nodiscard]] bool published_activity_transport_locked(
    std::uint64_t accountSoid,
    std::uint64_t characterSoid,
    std::uint64_t memberKey,
    middleware::bap::activity_message::TransportReport& report) noexcept;

/** Resolves a character's native address without conflating its fireteam hash with a member key. */
[[nodiscard]] bool published_character_transport_locked(
    std::uint64_t accountSoid,
    std::uint64_t characterSoid,
    middleware::bap::activity_message::TransportReport& report) noexcept;
} // namespace sunrise::server::bap
