#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../encoding/bit_writer.h"

namespace sunrise::middleware::bap::activity_message::incident {

/** Activity message type 19 carries one incident. Both sides can send it. */
inline constexpr std::uint32_t kMessageType = 19;

/** Every target index is 13 bits on the wire. */
inline constexpr std::uint8_t kTargetWidth = 13;
/** The highest valid target index. Above it the Client indexes handler tables unbounded. */
inline constexpr std::uint32_t kTargetMaximum = 7'762;
/** These three rows carry type code -1 and are a crash risk, so they never pass. */
inline constexpr std::array<std::uint32_t, 3> kPoisonTargets{795, 4'690, 5'375};

/** Extra targets follow a 5-bit count and stop at 25 entries. */
inline constexpr std::uint8_t kExtraCountWidth = 5;
inline constexpr std::uint32_t kExtraTargetMaximum = 25;
/** The selector is a presence bit, a 9-bit byte length, then at most 260 bytes. */
inline constexpr std::uint8_t kSelectorPresenceWidth = 1;
inline constexpr std::uint8_t kSelectorLengthWidth = 9;
inline constexpr std::uint32_t kSelectorMaximum = 260;
/** The optional field is a presence bit then 64 bits, sent as two 32-bit words. */
inline constexpr std::uint8_t kOptionalPresenceWidth = 1;
inline constexpr std::uint8_t kOptionalFieldWidth = 64;
inline constexpr std::uint8_t kOptionalWordWidth = 32;
/** The payload is a 9-bit byte length then at most 500 bytes. */
inline constexpr std::uint8_t kPayloadLengthWidth = 9;
inline constexpr std::uint32_t kPayloadMaximum = 500;
/** The smallest body is the five fixed fields with every count zero. */
inline constexpr std::size_t kMinimumBodyBits = kTargetWidth + kExtraCountWidth
                                                + kSelectorPresenceWidth + kOptionalPresenceWidth
                                                + kPayloadLengthWidth;
/** Largest body after every explicit target and optional byte field is present. */
inline constexpr std::size_t kMaximumBodyBits =
    kTargetWidth + kExtraCountWidth + kExtraTargetMaximum * kTargetWidth + kSelectorPresenceWidth
    + kSelectorLengthWidth + kSelectorMaximum * 8 + kOptionalPresenceWidth + kOptionalFieldWidth
    + kPayloadLengthWidth + kPayloadMaximum * 8;
/** Fixed byte capacity needed by the largest padded incident body. */
inline constexpr std::size_t kMaximumBodyBytes = (kMaximumBodyBits + 7) / 8;

/**
 * Bytes of the common incident metadata header (schemas 0x80809512 + 0x8080880D, 335 bits plus
 * one padding bit). Every payload schema in the incident family starts with it; the header-only
 * schemas 0x808087A5 / 0x808087AA are exactly this. An all-zero header (every biased field at its
 * absent value) is what makes a type-0 notification row such as 5330 display: verified in game.
 */
inline constexpr std::size_t kHeaderOnlyPayloadBytes = 42;

/** Why one incident did not pass validation. */
enum class Verdict : std::uint8_t {
    accepted,
    truncated,
    targetOutOfRange,
    targetPoisoned,
    tooManyTargets,
    payloadTooLong,
    selectorTooLong,
};

/**
 * One outer-valid incident, framed to the end of its payload.
 * The parser fills the outer fields only; the byte arrays and optional words are encoder inputs.
 */
struct Incident {
    std::array<std::byte, kSelectorMaximum> selector{};
    std::array<std::byte, kPayloadMaximum> payload{};
    std::uint32_t primaryTarget{};
    std::uint32_t extraTargets[kExtraTargetMaximum]{};
    std::uint32_t extraTargetCount{};
    std::uint32_t selectorLength{};
    std::uint32_t payloadLength{};
    std::uint32_t optionalWordA{};
    std::uint32_t optionalWordB{};
    /** Bits the body used. Below the payload's own bit count means trailing padding. */
    std::uint32_t consumedBits{};
    bool hasCompressedSelector{};
    /** Set when the two optional words are present. */
    bool hasOptionalBlock{};
};

/** @return A short stable name for one verdict, for the log line. */
[[nodiscard]] const char* verdict_name(Verdict verdict) noexcept;

/** @return True when every outer wire field is bounded; payload semantics are not checked. */
[[nodiscard]] bool outer_valid(const Incident& incident) noexcept;

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
 * Writes one bounded incident body after outer-field preflight.
 * The caller still owns activity identity, output ordering, nonce commit and transport staging.
 */
[[nodiscard]] bool write(encoding::bits::Writer& writer, const Incident& incident) noexcept;

} // namespace sunrise::middleware::bap::activity_message::incident
