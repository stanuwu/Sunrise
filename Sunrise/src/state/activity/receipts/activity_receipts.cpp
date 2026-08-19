#include "activity_receipts.h"

#include <Windows.h>

#include "../../runtime/storage/internal.h"

namespace sunrise::state::activity::receipts {
namespace {

/** @return The row one message type is counted in. Types above the table share the overflow row. */
[[nodiscard]] std::size_t row_for(std::uint32_t messageType) noexcept {
    return messageType < kOverflowRow ? static_cast<std::size_t>(messageType) : kOverflowRow;
}

} // namespace

/** Records one arriving activity message. */
std::uint32_t record(const Arrival& arrival) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ReceiptRegistry& registry = runtime::storage::g_state.activity.receipts;
    // The counter orders arrivals and nothing reads it as a clock, so wrapping loses only order
    // across a wrap and never invalidates a row.
    ++registry.sequence;
    const std::uint32_t sequence = registry.sequence;
    ActivityReceipt& row = registry.rows[row_for(arrival.messageType)];
    ++row.arrivals;
    row.lastSequence = sequence;
    row.lastSessionId = arrival.sessionId;
    row.lastPayloadBytes = arrival.payloadBytes;
    row.lastPeerHeardMask = arrival.peerHeardMask;
    row.lastConsumedBits = arrival.consumedBits;
    row.lastVerdict = arrival.verdict;
    if (arrival.verdict != Verdict::framed) {
        ++row.unframed;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return sequence;
}

/** Copies one message type's arrival record. */
bool snapshot(std::uint32_t messageType, ActivityReceipt& receipt) noexcept {
    receipt = {};
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityReceipt& row =
        runtime::storage::g_state.activity.receipts.rows[row_for(messageType)];
    const bool seen = row.arrivals != 0;
    if (seen) {
        receipt = row;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return seen;
}

/** Counts messages recorded with a verdict other than framed. */
std::uint32_t unframed_total() noexcept {
    std::uint32_t total = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const ActivityReceipt& row : runtime::storage::g_state.activity.receipts.rows) {
        total += row.unframed;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return total;
}

} // namespace sunrise::state::activity::receipts
