#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "entity_slots.h"

namespace sunrise::middleware::bap::activity_message::entity_authority {

/** The Client abandons slots for bubbles it has left. */
inline constexpr std::uint32_t kAbandonMessageType = 26;
/** The Client asks the host to purge slots it could not claim. */
inline constexpr std::uint32_t kRequestPurgeMessageType = 27;
/** The Client acknowledges an authority-mask reset. */
inline constexpr std::uint32_t kResetAcknowledgementMessageType = 29;
/** The Client answers a mask query with one selector's slots. */
inline constexpr std::uint32_t kQueryPerBubbleMessageType = 31;
/** The Client answers a mask query with its whole mask. */
inline constexpr std::uint32_t kQueryResponseMessageType = 32;
/** The Client gives up authority over a set of slots. */
inline constexpr std::uint32_t kAbdicateMessageType = 33;

/** Msgs 26 and 33 lead with a compact bubble selector byte. */
inline constexpr std::uint8_t kSelectorWidth = 8;
/** The reason on msg 26 and the leading field on msg 27 are 3 bits, stored as local + 1. */
inline constexpr std::uint8_t kReasonWidth = 3;
/** The 3-bit reason stores local + 1, so the local range is -1 to 6. */
inline constexpr std::int32_t kReasonBias = 1;
/** Msg 26 is one selector byte, the mask, then the 3-bit reason. */
inline constexpr std::size_t kAbandonBits =
    kSelectorWidth + entity_slots::kSlotCount + kReasonWidth;
/** Msg 27 is the 3-bit field then the mask, so its mask is not byte aligned. */
inline constexpr std::size_t kRequestPurgeBits = kReasonWidth + entity_slots::kSlotCount;
/** Msg 33 is one selector byte then the mask. */
inline constexpr std::size_t kAbdicateBits = kSelectorWidth + entity_slots::kSlotCount;
/** Msgs 29, 31 and 32 lead with the 4-byte correlation the host sent on msg 28 or 30. */
inline constexpr std::size_t kCorrelationSize = sizeof(std::uint32_t);

/** One decoded slot-release message. Msg 33 carries no reason. */
struct Release {
    entity_slots::EntitySlotMask mask{};
    std::uint8_t selector{};
    std::int32_t reason{};
    bool hasReason{};
};

/** One decoded answer to a host mask query or reset. */
struct QueryAnswer {
    entity_slots::EntitySlotMask mask{};
    std::uint32_t correlation{};
    std::uint8_t selector{};
    bool hasSelector{};
    bool hasMask{};
};

/**
 * Parses msg 26, whose mask starts on a byte boundary after the selector.
 * @param payload Activity message payload after the 17-byte envelope.
 * @param release Cleared first. Receives the selector, mask and reason on success.
 * @return True when the whole fixed body is present.
 */
[[nodiscard]] bool parse_abandon(std::span<const std::byte> payload, Release& release) noexcept;

/**
 * Parses msg 33, which is the selector and the mask with no reason.
 * @param payload Activity message payload after the 17-byte envelope.
 * @param release Cleared first. Receives the selector and mask on success.
 * @return True when the whole fixed body is present.
 */
[[nodiscard]] bool parse_abdicate(std::span<const std::byte> payload, Release& release) noexcept;

/**
 * Reads only the 3-bit leading field of msg 27. The mask that follows is not byte aligned, and
 * this host takes no action on a purge request, so it is not decoded.
 * @param payload Activity message payload after the 17-byte envelope.
 * @param reason Receives the debiased leading value.
 * @return True when the whole fixed body is present.
 */
[[nodiscard]] bool parse_request_purge(std::span<const std::byte> payload,
                                       std::int32_t& reason) noexcept;

/**
 * Parses msg 29, 31 or 32. Msg 29 is the correlation alone, msg 31 adds a selector and a mask,
 * and msg 32 adds a mask.
 * @param messageType One of 29, 31 or 32.
 * @param payload Activity message payload after the 17-byte envelope.
 * @param answer Cleared first. Receives the fields the message type carries.
 * @return True when the whole body for that type is present.
 */
[[nodiscard]] bool parse_query_answer(std::uint32_t messageType,
                                      std::span<const std::byte> payload,
                                      QueryAnswer& answer) noexcept;

} // namespace sunrise::middleware::bap::activity_message::entity_authority
