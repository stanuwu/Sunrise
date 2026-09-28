#include <algorithm>
#include <array>
#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode702.h"

namespace sunrise::middleware::web_service::messages::opcode702 {
namespace {
using encoding::bits::Reader;

/** Primitive widths in the character writeback schema. */
constexpr std::uint8_t kPresenceBits = 1;
constexpr std::uint8_t kByteBits = 8;
constexpr std::uint8_t kShortBits = 16;
constexpr std::uint8_t kWordBits = 32;
constexpr std::uint8_t kLongBits = 64;
constexpr std::uint8_t kJoinLockBits = 5;
/** Fixed array lengths from the character writeback schema. */
constexpr std::size_t kHeaderFloats = 3;
constexpr std::size_t kSeenWords = 4;
constexpr std::size_t kRosterItems = 20;
constexpr std::size_t kRosterPlugs = 64;
/** One roster row: a u64 key, two biased shorts, the plug name units, and a 4-bit tail. */
constexpr std::size_t kRosterRowSize = 0x90;
constexpr std::size_t kRosterRowFirstShort = 8;
constexpr std::size_t kRosterRowSecondShort = 10;
constexpr std::size_t kRosterRowName = 12;
constexpr std::size_t kRosterRowTail = kRosterRowName + kRosterPlugs * 2;
constexpr std::uint8_t kRosterRowTailBits = 4;
/** Signed shorts reach the wire biased by the 16-bit midpoint. */
constexpr std::uint64_t kShortBias = 0x8000;
constexpr std::size_t kOpaqueBytes = 128;
constexpr std::size_t kInventoryRows = 350;
constexpr std::size_t kUnlockFlags = 768;
constexpr std::size_t kTailBooleans = 3;

/** An absent field has no payload. */
template <typename Read> bool optional(Reader& reader, Read read) noexcept {
    std::uint64_t present = 0;
    return reader.read(kPresenceBits, present) && (present == 0 || read(reader));
}

bool skip_optional(Reader& reader, std::size_t width) noexcept {
    return optional(reader, [width](Reader& field) noexcept { return field.skip(width); });
}

/** Header floats carry their own presence bits; hash-array words do not. */
bool read_header(Reader& reader) noexcept {
    const auto floats = [](Reader& array) noexcept {
        for (std::size_t index = 0; index < kHeaderFloats; ++index) {
            if (!skip_optional(array, kWordBits)) {
                return false;
            }
        }
        return true;
    };
    return optional(reader, floats) && skip_optional(reader, kSeenWords * kWordBits)
           && skip_optional(reader, kWordBits);
}

/** The five-byte activity block packs its fields without inner presence bits. */
bool read_activity(Reader& reader, Request& output) noexcept {
    output.presence.hasGroup = true;
    // Three biased bytes and one three-bit selector precede the fireteam join-lock mask
    // (class 0x808079C6 ordinal 4, five bits, no bias), which nothing here consumes.
    constexpr std::size_t kBlockBits = 3 * kByteBits + 3 + kJoinLockBits;
    const auto groupKey = [&output](Reader& field) noexcept {
        std::uint64_t value{};
        if (!field.read(kWordBits, value)) {
            return false;
        }
        output.presence.groupKey = static_cast<std::uint32_t>(value) ^ 0x80000000U;
        return true;
    };
    const auto memberCount = [&output](Reader& field) noexcept {
        std::uint64_t value{};
        if (!field.read(kByteBits, value)) {
            return false;
        }
        output.presence.memberCount =
            static_cast<std::int8_t>(static_cast<std::uint8_t>(value) ^ 0x80U);
        return true;
    };
    return skip_optional(reader, kBlockBits) && optional(reader, groupKey)
           && optional(reader, memberCount) && skip_optional(reader, kLongBits);
}

/** The roster mirror has twenty records and seven trailing optional scalars. */
bool read_roster(Reader& reader, Request& output) noexcept {
    output.presence.hasFireteam = true;
    const auto store =
        [&output](std::size_t offset, std::size_t size, std::uint64_t value) noexcept {
            for (std::size_t i = 0; i < size; ++i) {
                output.presence.fireteam[offset + i] = static_cast<std::byte>(value & 0xFFU);
                value >>= 8;
            }
        };
    const auto scalar = [&store](Reader& field,
                                 std::size_t offset,
                                 std::size_t size,
                                 std::uint8_t width,
                                 std::uint64_t bias) noexcept {
        return optional(field, [&](Reader& present) noexcept {
            std::uint64_t value{};
            if (!present.read(width, value)) {
                return false;
            }
            store(offset, size, value - bias);
            return true;
        });
    };
    const auto items = [&scalar, &store](Reader& array) noexcept {
        for (std::size_t index = 0; index < kRosterItems; ++index) {
            const auto row = index * kRosterRowSize;
            const auto name = [&](Reader& units) noexcept {
                for (std::size_t unit = 0; unit < kRosterPlugs; ++unit) {
                    std::uint64_t value{};
                    if (!units.read(kShortBits, value)) {
                        return false;
                    }
                    store(row + kRosterRowName + unit * 2, 2, value - kShortBias);
                }
                return true;
            };
            if (!scalar(array, row, 8, kLongBits, 0)
                || !scalar(array, row + kRosterRowFirstShort, 2, kShortBits, kShortBias)
                || !scalar(array, row + kRosterRowSecondShort, 2, kShortBits, kShortBias)
                || !optional(array, name)
                || !scalar(array, row + kRosterRowTail, 1, kRosterRowTailBits, 0)) {
                return false;
            }
        }
        return true;
    };
    // The roster rows fill the record ahead of its tail, which is then two 64-bit ids, two
    // quantized floats and three byte-wide enums. The two signed enums carry the 8-bit midpoint
    // bias and the last is a 4-bit value at bias one.
    constexpr std::size_t kTailBase = kRosterItems * kRosterRowSize;
    constexpr std::uint64_t kByteBias = 0x80;
    constexpr std::array<std::uint8_t, 7> kTailWidths{
        kLongBits, kLongBits, kWordBits, kWordBits, kByteBits, kByteBits, kRosterRowTailBits};
    constexpr std::array<std::size_t, 7> kTailOffsets{kTailBase,
                                                      kTailBase + 8,
                                                      kTailBase + 0x10,
                                                      kTailBase + 0x14,
                                                      kTailBase + 0x18,
                                                      kTailBase + 0x19,
                                                      kTailBase + 0x1A};
    constexpr std::array<std::size_t, 7> kTailSizes{8, 8, 4, 4, 1, 1, 1};
    constexpr std::array<std::uint64_t, 7> kTailBiases{0, 0, 0, 0, kByteBias, kByteBias, 1};
    if (!optional(reader, items)) {
        return false;
    }
    for (std::size_t i = 0; i < kTailWidths.size(); ++i) {
        if (!scalar(reader, kTailOffsets[i], kTailSizes[i], kTailWidths[i], kTailBiases[i])) {
            return false;
        }
    }
    return true;
}

/** The leading byte counts the payload bytes, up to the buffer's capacity. */
bool read_opaque_state(Reader& reader, Request& output) noexcept {
    const auto buffer = [&output](Reader& fields) noexcept {
        std::uint64_t count = 0;
        if (!fields.read(kByteBits, count) || count > kOpaqueBytes) {
            return false;
        }
        output.presence.descriptorSize = static_cast<std::uint8_t>(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint64_t value{};
            if (!fields.read(kByteBits, value)) {
                return false;
            }
            output.presence.descriptor[i] = static_cast<std::byte>(value);
        }
        return true;
    };
    return optional(reader, buffer) && skip_optional(reader, kLongBits);
}

/** New-item bits and instance watermarks share a group but have independent presence bits. */
bool read_inventory(Reader& reader, Request& output) noexcept {
    const auto bitmap = [&output](Reader& array) noexcept {
        state::account::inventory::CharacterNewItems bits{};
        for (auto& word : bits) {
            std::uint64_t value = 0;
            if (!array.read(kWordBits, value)) {
                return false;
            }
            word = static_cast<std::uint32_t>(value);
        }
        output.newItems = bits;
        return true;
    };
    return optional(reader, bitmap) && skip_optional(reader, kInventoryRows * kWordBits);
}

/** Each boolean in the final array has its own presence bit. */
bool read_tail_booleans(Reader& reader) noexcept {
    for (std::size_t index = 0; index < kTailBooleans; ++index) {
        if (!skip_optional(reader, 1)) {
            return false;
        }
    }
    return true;
}

/** Child order follows the 5328-byte character bank. */
bool read_body(Reader& reader, Request& output) noexcept {
    // The next record contains six shorts, one word, and one long.
    constexpr std::size_t kFixedRecordBits = 6 * kShortBits + kWordBits + kLongBits;
    constexpr std::size_t kUnlockFlagBits = 3;
    return optional(reader,
                    [&output](Reader& group) noexcept { return read_activity(group, output); })
           && optional(reader,
                       [&output](Reader& group) noexcept { return read_roster(group, output); })
           && optional(
               reader,
               [&output](Reader& group) noexcept { return read_opaque_state(group, output); })
           && skip_optional(reader, kWordBits)
           && optional(reader,
                       [&output](Reader& group) noexcept { return read_inventory(group, output); })
           && skip_optional(reader, kFixedRecordBits)
           && skip_optional(reader, kUnlockFlags * kUnlockFlagBits)
           && optional(reader, read_tail_booleans);
}

/** Unused bytes in the fixed-capacity request buffer must be zero. */
bool zero_padding(Reader& reader) noexcept {
    while (reader.remaining_bits() != 0) {
        std::uint64_t value = 0;
        const auto width =
            static_cast<std::uint8_t>((std::min)(reader.remaining_bits(), std::size_t{kLongBits}));
        if (!reader.read(width, value) || value != 0) {
            return false;
        }
    }
    return true;
}
} // namespace

/** Reads activity state and new-item flags without applying a partial writeback. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.empty()
        || message.payload.size() > kPayloadSize) {
        return false;
    }
    Reader reader(message.payload);
    Request candidate;
    if (!optional(reader, read_header)
        || !optional(reader,
                     [&candidate](Reader& group) noexcept { return read_body(group, candidate); })
        || !zero_padding(reader)) {
        return false;
    }
    candidate.presence.published = true;
    request = candidate;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode702
