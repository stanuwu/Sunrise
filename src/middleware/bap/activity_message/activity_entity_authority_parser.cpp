/**
 * The Client sends msgs 26, 27, 29, 31, 32 and 33. It never receives them.
 * Msgs 26 and 33 return slots this host leased out, so both reach the lease store.
 * Msgs 27, 29, 31 and 32 answer or ask for host state this build does not keep, so they are read
 * and reported only. Msg 27's mask starts at bit 3 and is skipped for that reason.
 */

#include <algorithm>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "entity_authority.h"

namespace sunrise::middleware::bap::activity_message::entity_authority {
namespace {

/** The selector byte and the mask that follows it, shared by msgs 26 and 33. */
[[nodiscard]] bool read_selector_and_mask(std::span<const std::byte> payload,
                                          Release& release) noexcept {
    /** The compact bubble selector is one byte, so the mask behind it stays byte aligned. */
    constexpr std::size_t kSelectorSize = 1;
    if (payload.size() < kSelectorSize + entity_slots::kEncodedSize) {
        return false;
    }
    release.selector = std::to_integer<std::uint8_t>(payload.front());
    std::copy_n(payload.begin() + kSelectorSize, entity_slots::kEncodedSize, release.mask.begin());
    return true;
}

} // namespace

/** Parses msg 26, whose mask starts on a byte boundary after the selector. */
bool parse_abandon(std::span<const std::byte> payload, Release& release) noexcept {
    release = {};
    if (!read_selector_and_mask(payload, release)) {
        return false;
    }
    // The reason trails the mask, so the reader only has to reach the last 3 bits.
    encoding::bits::Reader reader(payload);
    std::uint64_t stored = 0;
    if (!reader.skip(kSelectorWidth + entity_slots::kSlotCount)
        || !reader.read(kReasonWidth, stored)) {
        release = {};
        return false;
    }
    release.reason = static_cast<std::int32_t>(stored) - kReasonBias;
    release.hasReason = true;
    return true;
}

/** Parses msg 33, which is the selector and the mask with no reason. */
bool parse_abdicate(std::span<const std::byte> payload, Release& release) noexcept {
    release = {};
    if (!read_selector_and_mask(payload, release)) {
        release = {};
        return false;
    }
    return true;
}

/** Reads only the 3-bit leading field of msg 27. */
bool parse_request_purge(std::span<const std::byte> payload, std::int32_t& reason) noexcept {
    reason = 0;
    encoding::bits::Reader reader(payload);
    std::uint64_t stored = 0;
    if (!reader.read(kReasonWidth, stored) || !reader.skip(entity_slots::kSlotCount)) {
        return false;
    }
    reason = static_cast<std::int32_t>(stored) - kReasonBias;
    return true;
}

/** Parses msg 29, 31 or 32. */
bool parse_query_answer(std::uint32_t messageType,
                        std::span<const std::byte> payload,
                        QueryAnswer& answer) noexcept {
    answer = {};
    if (payload.size() < kCorrelationSize) {
        return false;
    }
    answer.correlation = encoding::read_u32_be(payload.first<kCorrelationSize>());
    if (messageType == kResetAcknowledgementMessageType) {
        return true;
    }

    std::size_t offset = kCorrelationSize;
    if (messageType == kQueryPerBubbleMessageType) {
        if (payload.size() < offset + 1) {
            answer = {};
            return false;
        }
        answer.selector = std::to_integer<std::uint8_t>(payload[offset]);
        answer.hasSelector = true;
        offset += 1;
    } else if (messageType != kQueryResponseMessageType) {
        answer = {};
        return false;
    }

    if (payload.size() < offset + entity_slots::kEncodedSize) {
        answer = {};
        return false;
    }
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                entity_slots::kEncodedSize,
                answer.mask.begin());
    answer.hasMask = true;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::entity_authority
