#pragma once

#include <cstdint>
#include <span>

#include "../../../middleware/bap/activity_message/sense_update.h"

// Reads one decoded type-70 engagement report into a membership snapshot. The report lists the
// player keys the client admitted; whether the bound player is among them is the only membership
// fact the single-player server needs. The 16-bit value is retained unnamed.

namespace sunrise::server::activity::mission::engagement_observation {

/** Root and list schemas the Sense body decoder records the report under. */
inline constexpr std::uint32_t kRootSchema = 0x808094F0U;
inline constexpr std::uint32_t kListSchema = 0x808094F8U;
/** List ordinal 0 is the five-bit count; ordinal 1 the key array, one occurrence per key. */
inline constexpr std::uint16_t kCountOrdinal = 0;
inline constexpr std::uint16_t kKeyOrdinal = 1;
/** Root ordinal 1 is the 16-bit biased value that follows the list. */
inline constexpr std::uint16_t kValueOrdinal = 1;
/** The native list capacity; the decoder refuses larger counts before any key is read. */
inline constexpr std::int32_t kCapacity = 16;

/** Bounded membership facts decoded from one type-70 Sense body. */
struct Snapshot final {
    /** Participants the client reported. */
    std::int32_t count{};
    /** Keys the decoder retained for this report. */
    std::int32_t keysDecoded{};
    /** The biased 16-bit field; its meaning is not recovered. */
    std::int32_t value{};
    /** The bound player's key equals one retained key. */
    bool localMember{};
    /** The count was present and every reported key was retained. */
    bool complete{};
};

/**
 * @param body The values the Sense decoder retained for one type-70 object.
 * @param playerKey The bound link's message-5 player key.
 * @return False when the body carries no engagement count (for example, a root bit of zero).
 */
[[nodiscard]] inline bool
read(std::span<const middleware::bap::activity_message::sense_update::DecodedValue> body,
     std::uint64_t playerKey,
     Snapshot& output) noexcept {
    output = {};
    bool countSeen = false;
    bool valueSeen = false;
    for (const auto& value : body) {
        if (!value.present) {
            continue;
        }
        if (value.schemaRow == kListSchema && value.fieldOrdinal == kCountOrdinal) {
            if (value.unsignedValue > static_cast<std::uint64_t>(kCapacity)) {
                return false;
            }
            output.count = static_cast<std::int32_t>(value.unsignedValue);
            countSeen = true;
        } else if (value.schemaRow == kListSchema && value.fieldOrdinal == kKeyOrdinal) {
            if (output.keysDecoded < kCapacity) {
                ++output.keysDecoded;
            }
            if (playerKey != 0 && value.unsignedValue == playerKey) {
                output.localMember = true;
            }
        } else if (value.schemaRow == kRootSchema && value.fieldOrdinal == kValueOrdinal) {
            output.value = static_cast<std::int32_t>(value.signedValue);
            valueSeen = true;
        }
    }
    if (!countSeen) {
        return false;
    }
    output.complete = valueSeen && output.keysDecoded == output.count;
    return true;
}

} // namespace sunrise::server::activity::mission::engagement_observation
