#include "activity_join_request_parser.h"

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message::join_request {
namespace {

/** Fixed typed prefix required from the activity join-request payload. */
struct JoinRequestLayout final {
    /** The big-endian request correlation starts the payload. */
    static constexpr std::size_t correlation = 0;
    /** The big-endian activity session follows the correlation. */
    static constexpr std::size_t sessionId = correlation + encoding::kU32Size;
    /** 8 key bytes, low byte first, follow the activity session. */
    static constexpr std::size_t memberKey = sessionId + encoding::kU64Size;
    /** Later join fields start after the required member key. */
    static constexpr std::size_t prefixSize = memberKey + encoding::kU64Size;
};

/**
 * Bit offset of the signed-in character inside the join request. Every field ahead of it is
 * fixed width with no presence bit, so the offset is always the same.
 */
constexpr std::size_t kCharacterSoidBit = 330;
/** The character id is a full 64-bit value, most significant bit first. */
constexpr std::uint8_t kCharacterSoidWidth = 64;

/**
 * Reads the signed-in character, leaving it zero when the payload stops short of the field. A
 * short payload is normal here: only the fixed prefix above is required, and the roster falls
 * back to the selected character when this is missing.
 * @param input Complete join-request activity payload.
 * @return The character id, or zero when it is not there.
 */
[[nodiscard]] std::uint64_t read_character_soid(std::span<const std::byte> input) noexcept {
    encoding::bits::Reader reader(input);
    std::uint64_t value = 0;
    if (!reader.skip(kCharacterSoidBit) || !reader.read(kCharacterSoidWidth, value)) {
        return 0;
    }
    return value;
}

} // namespace

/** Reads the correlation, nonzero session, and member key from an activity join request. */
bool parse_join_request(std::span<const std::byte> input, JoinRequest& request) noexcept {
    request = {};
    if (input.size() < JoinRequestLayout::prefixSize) {
        return false;
    }

    JoinRequest parsed{};
    parsed.correlation =
        encoding::read_u32_be(input.subspan<JoinRequestLayout::correlation, encoding::kU32Size>());
    parsed.sessionId =
        encoding::read_u64_be(input.subspan<JoinRequestLayout::sessionId, encoding::kU64Size>());
    parsed.memberKey =
        encoding::read_u64_le(input.subspan<JoinRequestLayout::memberKey, encoding::kU64Size>());
    if (parsed.sessionId == kAbsentSessionId) {
        return false;
    }
    parsed.characterSoid = read_character_soid(input);

    request = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::join_request
