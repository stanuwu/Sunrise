#include "opcode501_request_codec.h"

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The identity triple and appearance header end here; the 64-byte name follows. Only the
 * triple is decoded; the rest is skipped as one span. */
constexpr std::size_t kIdentityAndHeaderBits = 261;
/** Each triple entry is a 1-bit presence flag plus an 8-bit +0x80-biased value. */
constexpr std::uint8_t kTripleEntryBits = 9;
constexpr std::uint64_t kByteBias = 0x80U;
/** Bits left to skip after the triple: the appearance header and the request's trailing field. */
constexpr std::size_t kSkipBits = kIdentityAndHeaderBits - 3U * kTripleEntryBits;

/**
 * Reads one 1-bit presence flag followed by an 8-bit +0x80-biased value.
 * @return True when the field is present and readable. An absent field fails closed: nothing on
 *         the wire carries the value that was skipped, so there is nothing to fall back to.
 */
[[nodiscard]] bool read_optional_byte(encoding::bits::Reader& reader, std::uint8_t& value) noexcept {
    std::uint64_t present = 0;
    if (!reader.read(1, present) || present == 0) {
        return false;
    }
    std::uint64_t raw = 0;
    if (!reader.read(8, raw)) {
        return false;
    }
    value = static_cast<std::uint8_t>(raw - kByteBias);
    return true;
}

/** Highest valid race, gender and class wire values, matching the state enums. */
constexpr std::uint8_t kMaxRace = 2;
constexpr std::uint8_t kMaxGender = 1;
constexpr std::uint8_t kMaxClass = 2;

} // namespace

bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() * 8U < kIdentityAndHeaderBits) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    if (!read_optional_byte(reader, request.race) || !read_optional_byte(reader, request.gender)
        || !read_optional_byte(reader, request.characterClass)) {
        return false;
    }
    if (request.race > kMaxRace || request.gender > kMaxGender
        || request.characterClass > kMaxClass) {
        return false;
    }
    return reader.skip(kSkipBits);
}

} // namespace sunrise::middleware::web_service::messages::opcode501
