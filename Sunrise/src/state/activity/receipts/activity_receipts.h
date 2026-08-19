#pragma once

#include <cstddef>
#include <cstdint>

#include "definition.h"

namespace sunrise::state::activity::receipts {

/** One arriving activity message, as the router framed it. */
struct Arrival {
    std::uint64_t sessionId{};
    std::uint32_t messageType{};
    std::uint32_t payloadBytes{};
    std::uint32_t peerHeardMask{};
    /** Bits the parser consumed. Zero when the type has no parser of its own. */
    std::uint32_t consumedBits{};
    Verdict verdict{Verdict::framed};
};

/**
 * Records one arriving activity message.
 * Every routed message calls this, including the ones that change nothing, so a type that stops
 * arriving is visible instead of silent.
 * @param arrival Framing facts for the message that just arrived.
 * @return The registry-wide arrival order stamped into the row.
 */
std::uint32_t record(const Arrival& arrival) noexcept;

/**
 * Copies one message type's arrival record.
 * TODO: no reader yet. The diagnostics overlay has to show these rows before this is called.
 * @param messageType Activity message type.
 * @param receipt Cleared, then filled from the row.
 * @return True when a message of that type has arrived.
 */
[[nodiscard]] bool snapshot(std::uint32_t messageType, ActivityReceipt& receipt) noexcept;

/** @return Messages recorded with a verdict other than framed, across every type. */
[[nodiscard]] std::uint32_t unframed_total() noexcept;

} // namespace sunrise::state::activity::receipts
