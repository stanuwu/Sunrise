#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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
/** One bit says whether optional field K follows. */
inline constexpr std::uint8_t kOptionalPresenceWidth = 1;
/** Optional field K is two 32-bit words. */
inline constexpr std::uint8_t kOptionalFieldWidth = 64;
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
};

/** One validated incident. Fields after a compressed selector are not decoded. */
struct Incident {
    std::uint32_t primaryTarget{};
    std::uint32_t extraTargets[kExtraTargetMaximum]{};
    std::uint32_t extraTargetCount{};
    std::uint32_t payloadLength{};
    /** Set when a compressed selector follows, which ends decoding for this body. */
    bool hasCompressedSelector{};
    /** Set when the payload length and its bytes were reached and checked. */
    bool hasPayload{};
};

/** @return A short stable name for one verdict, for the log line. */
[[nodiscard]] const char* verdict_name(Verdict verdict) noexcept;

/**
 * Validates one msg-19 body as far as its wire shape allows.
 * Every target index is range and poison checked. Decoding stops at a compressed target selector,
 * whose wire length is not recoverable from this artifact, so the payload behind one is not read.
 * @param payload Activity message payload after the 17-byte envelope.
 * @param parsed Cleared first. Receives every field reached before the verdict.
 * @return accepted, or the first rule the body broke.
 */
[[nodiscard]] Verdict validate(std::span<const std::byte> payload, Incident& parsed) noexcept;

} // namespace sunrise::middleware::bap::activity_message::incident
