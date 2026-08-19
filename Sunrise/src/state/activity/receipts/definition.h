#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity::receipts {

/**
 * Rows the registry holds, one per activity message type.
 * The recovered types run 0 through 54, so 64 covers every one of them and any type a later build
 * adds below the same ceiling. A type above it is counted in the overflow row instead of dropped.
 */
inline constexpr std::size_t kReceiptCapacity = 64;
/** Row every message type at or above the capacity is counted in. */
inline constexpr std::size_t kOverflowRow = kReceiptCapacity - 1;

/** How completely one arriving activity message was framed. */
enum class Verdict : std::uint8_t {
    /** No message of this type has arrived. */
    absent,
    /** Every field the type declares was read and the body ended where it should. */
    framed,
    /**
     * Framed to a bounded point, with the rest of the body retained as one ambiguous tail.
     * The tail is not claimed to be individually understood.
     */
    partial,
    /** The declared framing did not hold, so no field of this body may be trusted. */
    malformed,
    /** Well formed and deliberately not acted on. */
    quarantined,
    /** The envelope named a session this link does not own. */
    unowned,
};

/**
 * One activity message type's arrival record.
 * It holds framing facts only. Typed values a message changes live in the domain that owns them,
 * so nothing here has to be kept in step with a wire body.
 */
struct ActivityReceipt {
    /** Session the last message of this type named, which is the envelope's own handle. */
    std::uint64_t lastSessionId{};
    /** Messages of this type this process has seen. */
    std::uint32_t arrivals{};
    /** Registry-wide arrival order of the last one, so two types can be ordered against each other.
     */
    std::uint32_t lastSequence{};
    /** Declared payload bytes of the last one. */
    std::uint32_t lastPayloadBytes{};
    /** Peer-heard mask the last envelope carried. */
    std::uint32_t lastPeerHeardMask{};
    /** Bits the parser consumed. Below the payload's bit count means a tail was left unread. */
    std::uint32_t lastConsumedBits{};
    /** Arrivals whose verdict was not `framed`. */
    std::uint32_t unframed{};
    Verdict lastVerdict{Verdict::absent};
};

/** Every activity message type's arrival record, indexed by message type. */
struct ReceiptRegistry {
    std::array<ActivityReceipt, kReceiptCapacity> rows{};
    /** Arrival counter stamped into every row. It orders arrivals and never has to be a clock. */
    std::uint32_t sequence{};
};

} // namespace sunrise::state::activity::receipts
