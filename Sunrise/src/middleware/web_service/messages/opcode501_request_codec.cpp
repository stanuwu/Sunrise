#include "opcode501_request_codec.h"

#include <array>
#include <cstring>
#include <type_traits>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::web_service::messages::opcode501 {
namespace {

/** The identity triple and appearance header end here; the 64-byte name follows. */
constexpr std::size_t kIdentityAndHeaderBits = 261;
/** Each identity byte is preceded by a 1-bit presence flag. */
constexpr std::uint8_t kPresenceBits = 1;
/** Header fields travel as 8, 16 or 32 bits. */
constexpr std::uint8_t kByteBits = 8;
constexpr std::uint8_t kWordBits = 16;
constexpr std::uint8_t kHashBits = 32;
/** The request's last header field is 2 bits wide. */
constexpr std::uint8_t kTrailingBits = 2;
/** 8-bit fields travel biased by +0x80, 16-bit fields by +0x8000; 32-bit fields unbiased. */
constexpr std::uint64_t kByteBias = 0x80U;
constexpr std::uint64_t kWordBias = 0x8000U;
/** Each of the header's two index runs holds 3 entries. */
constexpr std::size_t kIndexRunCount = 3;
/** The record requires 1 in the last field; the request always carries 0 there. */
constexpr std::uint8_t kRecordTrailingValue = 1;
/** Padding after the last field fills the record to its 4-byte alignment. */
constexpr std::size_t kTailPadding = 3;

/** The appearance header in record byte order. Fields are named by their record offset. */
struct HeaderLayout {
    std::uint16_t word00;
    std::uint8_t byte02;
    std::uint8_t byte03;
    std::uint8_t byte04;
    std::uint8_t padding05;
    std::uint16_t word06;
    std::uint16_t word08;
    std::uint16_t word10;
    std::array<std::uint16_t, kIndexRunCount> indices12;
    std::array<std::uint16_t, kIndexRunCount> indices18;
    std::uint16_t word24;
    std::uint16_t padding26;
    std::uint32_t hash28;
    std::uint8_t trailing32;
    std::array<std::uint8_t, kTailPadding> padding33;
};
static_assert(sizeof(HeaderLayout) == state::kAppearanceHeaderSize);
static_assert(std::is_trivially_copyable_v<HeaderLayout>);

/**
 * Reads one presence flag and the 8-bit biased value behind it.
 * @param reader Request bit reader.
 * @param value Receives the unbiased value.
 * @return False when the field is absent or short. Nothing else carries an absent value.
 */
[[nodiscard]] bool read_present_byte(encoding::bits::Reader& reader, std::uint8_t& value) noexcept {
    std::uint64_t present = 0;
    std::uint64_t raw = 0;
    if (!reader.read(kPresenceBits, present) || present == 0 || !reader.read(kByteBits, raw)) {
        return false;
    }
    value = static_cast<std::uint8_t>(raw - kByteBias);
    return true;
}

/** Reads one 8-bit +0x80-biased header field. @return False when the payload is short. */
[[nodiscard]] bool read_byte(encoding::bits::Reader& reader, std::uint8_t& value) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(kByteBits, raw)) {
        return false;
    }
    value = static_cast<std::uint8_t>(raw - kByteBias);
    return true;
}

/** Reads one 16-bit +0x8000-biased header field. @return False when the payload is short. */
[[nodiscard]] bool read_word(encoding::bits::Reader& reader, std::uint16_t& value) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(kWordBits, raw)) {
        return false;
    }
    value = static_cast<std::uint16_t>(raw - kWordBias);
    return true;
}

/**
 * Reads the appearance header fields in wire order.
 * @param reader Request bit reader, just past the identity triple.
 * @param header Receives the fields; padding stays zero.
 * @return False when the payload is short.
 */
[[nodiscard]] bool read_header(encoding::bits::Reader& reader, HeaderLayout& header) noexcept {
    if (!read_word(reader, header.word00) || !read_byte(reader, header.byte02)
        || !read_byte(reader, header.byte03) || !read_byte(reader, header.byte04)
        || !read_word(reader, header.word06) || !read_word(reader, header.word08)
        || !read_word(reader, header.word10)) {
        return false;
    }
    for (std::uint16_t& index : header.indices12) {
        if (!read_word(reader, index)) {
            return false;
        }
    }
    for (std::uint16_t& index : header.indices18) {
        if (!read_word(reader, index)) {
            return false;
        }
    }
    std::uint64_t hash = 0;
    std::uint64_t trailing = 0;
    if (!read_word(reader, header.word24) || !reader.read(kHashBits, hash)
        || !reader.read(kTrailingBits, trailing)) {
        return false;
    }
    header.hash28 = static_cast<std::uint32_t>(hash);
    // The request's own trailing value is read past but not kept.
    header.trailing32 = kRecordTrailingValue;
    return true;
}

} // namespace

/**
 * Parses the create-character request body: the identity triple, then the appearance header.
 * The trailing 64-byte name is not read.
 * @param message Parsed Web Service envelope.
 * @param request Receives the creation fields, the header in record byte order.
 * @return False when the payload is short or a presence flag is clear.
 */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() * kByteBits < kIdentityAndHeaderBits) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    HeaderLayout header{};
    if (!read_present_byte(reader, request.race) || !read_present_byte(reader, request.gender)
        || !read_present_byte(reader, request.characterClass) || !read_header(reader, header)) {
        request = {};
        return false;
    }
    std::memcpy(request.appearanceHeader.data(), &header, sizeof header);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode501
