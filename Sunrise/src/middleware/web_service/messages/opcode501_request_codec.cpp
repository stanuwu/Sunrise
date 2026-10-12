#include "opcode501_request_codec.h"

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The identity triple and appearance header end here; the 64-byte name follows. */
constexpr std::size_t kIdentityAndHeaderBits = 261;

constexpr std::uint64_t kByteBias = 0x80U;
constexpr std::uint64_t kWordBias = 0x8000U;

/** Writes the record's little-endian byte order, which is not the wire's bit order. */
void put8(state::AppearanceHeader& header, std::size_t offset, std::uint64_t value) noexcept {
    header[offset] = static_cast<std::uint8_t>(value & 0xFFU);
}

void put16(state::AppearanceHeader& header, std::size_t offset, std::uint64_t value) noexcept {
    header[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    header[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void put32(state::AppearanceHeader& header, std::size_t offset, std::uint64_t value) noexcept {
    put16(header, offset, value & 0xFFFFU);
    put16(header, offset + 2U, (value >> 16U) & 0xFFFFU);
}

/**
 * Reads one 1-bit presence flag followed by an 8-bit biased value.
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

/** Reads an 8-bit +0x80-biased header field straight into the record block. */
[[nodiscard]] bool
copy_byte(encoding::bits::Reader& reader, state::AppearanceHeader& header, std::size_t offset) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(8, raw)) {
        return false;
    }
    put8(header, offset, raw - kByteBias);
    return true;
}

/** Reads a 16-bit +0x8000-biased header field straight into the record block. */
[[nodiscard]] bool
copy_word(encoding::bits::Reader& reader, state::AppearanceHeader& header, std::size_t offset) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(16, raw)) {
        return false;
    }
    put16(header, offset, raw - kWordBias);
    return true;
}

/**
 * Byte offsets of every appearance-header field inside the 36-byte record block. The gaps at 5,
 * 26 and 33 are the struct's own alignment padding and stay zero.
 */
constexpr std::size_t kOffsetF0 = 0;
constexpr std::size_t kOffsetF2 = 2;
constexpr std::size_t kOffsetF3 = 3;
constexpr std::size_t kOffsetF4 = 4;
constexpr std::size_t kOffsetF6 = 6;
constexpr std::size_t kOffsetF8 = 8;
constexpr std::size_t kOffsetF10 = 10;
constexpr std::size_t kOffsetF12 = 12;
constexpr std::size_t kOffsetF18 = 18;
constexpr std::size_t kOffsetF24 = 24;
constexpr std::size_t kOffsetF28 = 28;
constexpr std::size_t kOffsetF32 = 32;

/** Both triples of 16-bit customization indices hold 3 entries. */
constexpr std::size_t kIndexTripleCount = 3;
/** Each entry in those triples is 2 bytes wide in the record block. */
constexpr std::size_t kIndexTripleStride = 2;

/** The record requires 1 in the trailing field; the request always carries 0 there. */
constexpr std::uint64_t kRecordTrailingField = 1;

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

    auto& header = request.appearanceHeader;
    if (!copy_word(reader, header, kOffsetF0) || !copy_byte(reader, header, kOffsetF2)
        || !copy_byte(reader, header, kOffsetF3) || !copy_byte(reader, header, kOffsetF4)
        || !copy_word(reader, header, kOffsetF6) || !copy_word(reader, header, kOffsetF8)
        || !copy_word(reader, header, kOffsetF10)) {
        return false;
    }
    for (std::size_t index = 0; index < kIndexTripleCount; ++index) {
        if (!copy_word(reader, header, kOffsetF12 + index * kIndexTripleStride)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < kIndexTripleCount; ++index) {
        if (!copy_word(reader, header, kOffsetF18 + index * kIndexTripleStride)) {
            return false;
        }
    }
    if (!copy_word(reader, header, kOffsetF24)) {
        return false;
    }
    std::uint64_t emptyHashSentinel = 0;
    if (!reader.read(32, emptyHashSentinel)) {
        return false;
    }
    put32(header, kOffsetF28, emptyHashSentinel);

    // Read but not copied: the request's own value is 0 and the record rejects it.
    std::uint64_t requestTrailingField = 0;
    if (!reader.read(2, requestTrailingField)) {
        return false;
    }
    put8(header, kOffsetF32, kRecordTrailingField);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode501
