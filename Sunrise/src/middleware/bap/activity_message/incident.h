#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::bap::activity_message::incident {

/** Activity message type 19 carries one incident. Both sides can send it. */
inline constexpr std::uint32_t kMessageType = 19;

/** Target indices are 13 bits and resolve through the 7,763-record global table. */
inline constexpr std::uint8_t kTargetWidth = 13;
/** The highest valid target index. Above it the Client indexes handler tables unbounded. */
inline constexpr std::uint32_t kTargetMaximum = 7'762;
/** These three rows carry type code -1 and are a crash risk, so they never pass. */
inline constexpr std::array<std::uint32_t, 3> kPoisonTargets{795, 4'690, 5'375};

/** The extra-target count is 5 bits, so the wire can ask for more than the limit allows. */
inline constexpr std::uint8_t kExtraCountWidth = 5;
/** At most 25 extra targets follow the primary one. */
inline constexpr std::uint32_t kExtraTargetMaximum = 25;
/** One bit says whether a compressed target selector follows. */
inline constexpr std::uint8_t kSelectorPresenceWidth = 1;
/** The selector byte length is 9 bits, so the wire can ask for more than the limit allows. */
inline constexpr std::uint8_t kSelectorLengthWidth = 9;
/** At most 260 selector bytes follow. Their meaning stays opaque. */
inline constexpr std::uint32_t kSelectorMaximum = 260;
/** One bit says whether optional field K follows. */
inline constexpr std::uint8_t kOptionalPresenceWidth = 1;
/** Optional field K is two 32-bit words. */
inline constexpr std::uint8_t kOptionalFieldWidth = 64;
/** Each optional word is a full 32-bit field. */
inline constexpr std::uint8_t kOptionalWordWidth = 32;
/** The payload byte length is 9 bits, so the wire can ask for more than the limit allows. */
inline constexpr std::uint8_t kPayloadLengthWidth = 9;
/** At most 500 payload bytes follow. */
inline constexpr std::uint32_t kPayloadMaximum = 500;
/** The smallest body is the five fixed fields with every count zero. */
inline constexpr std::size_t kMinimumBodyBits = kTargetWidth + kExtraCountWidth
                                                + kSelectorPresenceWidth + kOptionalPresenceWidth
                                                + kPayloadLengthWidth;

/** Why one incident did not pass validation. */
enum class Verdict : std::uint8_t {
    accepted,
    /** The body is shorter than the fields it declares. */
    truncated,
    /** A target index is above 7,762. */
    targetOutOfRange,
    /** A target index is one of the three type-code -1 rows. */
    targetPoisoned,
    /** More than 25 extra targets were declared. */
    tooManyTargets,
    /** More than 500 payload bytes were declared. */
    payloadTooLong,
    /** More than 260 selector bytes were declared. */
    selectorTooLong,
};

/** One validated incident, framed to the end of its payload. */
struct Incident {
    std::array<std::byte, kSelectorMaximum> selector{};
    std::array<std::byte, kPayloadMaximum> payload{};
    std::uint32_t primaryTarget{};
    std::uint32_t extraTargets[kExtraTargetMaximum]{};
    std::uint32_t extraTargetCount{};
    std::uint32_t selectorLength{};
    std::uint32_t payloadLength{};
    /** Both optional words, read only when the optional block is present. */
    std::uint32_t optionalWordA{};
    std::uint32_t optionalWordB{};
    /** Bits the body used. Below the payload's own bit count means trailing padding. */
    std::uint32_t consumedBits{};
    /** Set when a compressed selector is present. Its bytes stay opaque. */
    bool hasCompressedSelector{};
    /** Set when the two optional words are present. */
    bool hasOptionalBlock{};
    /** Set when the payload length and its bytes were reached and checked. */
    bool hasPayload{};
};

/** @return A short stable name for one verdict, for the log line. */
[[nodiscard]] const char* verdict_name(Verdict verdict) noexcept;

/**
 * Validates one incident body from its first target to the end of its payload.
 * Every target index is range and poison checked before anything else, because an out-of-range
 * index is a crash in the consumer rather than a decode error.
 * @param payload Activity message payload after the envelope.
 * @param parsed Cleared first. Receives every field reached before the verdict.
 * @return accepted, or the first rule the body broke.
 */
[[nodiscard]] Verdict validate(std::span<const std::byte> payload, Incident& parsed) noexcept;

/**
 * Writes one bounded incident body after a complete semantic preflight.
 * TODO: no sender yet. The outbound gameplay-event route has to open before this is called.
 */
[[nodiscard]] bool write(encoding::bits::Writer& writer, const Incident& incident) noexcept;

} // namespace sunrise::middleware::bap::activity_message::incident
