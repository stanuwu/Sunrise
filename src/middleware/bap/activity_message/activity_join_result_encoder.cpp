#include "activity_join_result_encoder.h"

#include <algorithm>

#include "../../encoding/byte_order.h"
#include "definition.h"

namespace sunrise::middleware::bap::activity_message::join_result {
namespace {

/** Fixed scalar fields echoed from the join request. */
struct JoinResultLayout final {
    /** The big-endian request correlation starts the result. */
    static constexpr std::size_t correlation = 0;
    /** The big-endian activity session follows the correlation. */
    static constexpr std::size_t sessionId = correlation + encoding::kU32Size;
};

/** The known minimal join result uses 6,053 meaningful bits inside 757 bytes. */
constexpr std::size_t kMeaningfulBitCount = 6'053;
/** The 3-bit join-status field starts at schema bit 570. */
constexpr std::size_t kJoinStatusBitOffset = 570;
/** The accepted stored value sits in the last bit of the 3-bit status field. */
constexpr std::size_t kJoinStatusBitCount = 3;
/** Stored join-status value 1 decodes to the accepted enum. */
constexpr std::uint8_t kAcceptedJoinStatus = 1;
/** The caller's keepalive hint starts at schema bit 1,781. */
constexpr std::size_t kKeepaliveBitOffset = 1'781;
/** The keepalive hint is one unsigned 16-bit schema field. */
constexpr std::size_t kKeepaliveBitCount = encoding::kU16Size * encoding::kBitsPerByte;

static_assert(kEncodedSize
              == (kMeaningfulBitCount + encoding::kBitsPerByte - 1) / encoding::kBitsPerByte);
static_assert(kJoinStatusBitOffset + kJoinStatusBitCount <= kMeaningfulBitCount);
static_assert(kKeepaliveBitOffset + kKeepaliveBitCount <= kMeaningfulBitCount);

/**
 * Writes one unsigned value into a zeroed most-significant-bit-first field.
 * @param output Whole join-result body, already zeroed.
 * @param bitOffset First destination schema bit.
 * @param bitCount How many low bits of the value to encode.
 */
void write_bits(std::span<std::byte> output,
                std::size_t bitOffset,
                std::size_t bitCount,
                std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < bitCount; ++index) {
        const std::size_t sourceShift = bitCount - index - 1;
        const auto sourceMask = static_cast<std::uint16_t>(1U << sourceShift);
        if ((value & sourceMask) == 0) {
            continue;
        }

        const std::size_t destinationBit = bitOffset + index;
        const std::size_t destinationByte = destinationBit / encoding::kBitsPerByte;
        const std::size_t destinationShift =
            encoding::kBitsPerByte - (destinationBit % encoding::kBitsPerByte) - 1;
        output[destinationByte] |= static_cast<std::byte>(1U << destinationShift);
    }
}

} // namespace

/** Encodes the minimal accepted join result from typed request scalars. */
bool encode_join_result(std::uint32_t correlation,
                        std::uint64_t sessionId,
                        std::uint16_t keepaliveHintMs,
                        std::span<std::byte> output,
                        std::size_t& written) noexcept {
    written = {};
    if (sessionId == kAbsentSessionId || output.size() < kEncodedSize) {
        return false;
    }

    const std::span body = output.first(kEncodedSize);
    std::fill(body.begin(), body.end(), std::byte{});
    encoding::write_u32_be(body.subspan<JoinResultLayout::correlation, encoding::kU32Size>(),
                           correlation);
    encoding::write_u64_be(body.subspan<JoinResultLayout::sessionId, encoding::kU64Size>(),
                           sessionId);
    write_bits(body, kJoinStatusBitOffset, kJoinStatusBitCount, kAcceptedJoinStatus);
    write_bits(body, kKeepaliveBitOffset, kKeepaliveBitCount, keepaliveHintMs);
    written = kEncodedSize;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::join_result
