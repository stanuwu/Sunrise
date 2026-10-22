#pragma once

#include "established_packet.h"

namespace sunrise::middleware::gameplay::peer::outbound_window {
/** The first bounded run stays unchanged until a carrying packet is acknowledged. */
[[nodiscard]] std::size_t count(const state::gameplay::OutboundQueue& queue) noexcept;
/** Stamps a new run; retransmissions preserve its original acknowledgement target. */
void begin(state::gameplay::OutboundQueue& queue, std::uint16_t packet) noexcept;
/** Removes only the acknowledged run, retaining later messages and fragment sequence numbers. */
[[nodiscard]] bool acknowledge(state::gameplay::OutboundQueue& queue, const AckState& ack) noexcept;
/** Reopens a first-send attempt that never left the transport. */
void send_failed(state::gameplay::OutboundQueue& queue, std::uint16_t packet) noexcept;
} // namespace sunrise::middleware::gameplay::peer::outbound_window
