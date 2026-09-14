#include "activity_join_result_encoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

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
/** The peer-heard window starts at schema bit 1,765, immediately before the keepalive hint. */
constexpr std::size_t kPeerHeardWindowBitOffset = 1'765;
/** The peer-heard window is one unsigned 16-bit schema field. */
constexpr std::size_t kPeerHeardWindowBitCount = encoding::kU16Size * encoding::kBitsPerByte;
/** The caller's keepalive hint starts at schema bit 1,781. */
constexpr std::size_t kKeepaliveBitOffset = 1'781;
/** The keepalive hint is one unsigned 16-bit schema field. */
constexpr std::size_t kKeepaliveBitCount = encoding::kU16Size * encoding::kBitsPerByte;
/** The host session text starts at schema bit 573 and holds 128 elements. */
constexpr std::size_t kHostSessionTextBitOffset = 573;
constexpr std::size_t kHostSessionTextByteCount = 128;
/** The OOPAH return code starts at schema bit 1,629 and is one signed 32-bit schema field. */
constexpr std::size_t kOopahReturnCodeBitOffset = 1'629;
constexpr std::size_t kOopahReturnCodeBitCount = 32;
/** The cached spare text starts at schema bit 1,893 and holds 256 elements. */
constexpr std::size_t kSpareTextBitOffset = 1'893;
constexpr std::size_t kSpareTextByteCount = 256;
/** The active workspace name starts at schema bit 4,005 and holds 256 elements. */
constexpr std::size_t kWorkspaceTextBitOffset = 4'005;
constexpr std::size_t kWorkspaceTextByteCount = 256;
/** Text elements carry a bias of 128, so raw 0x80 is the logical NUL and raw zero is filler. */
constexpr std::uint32_t kTextNulElement = 0x80;
/** The return code carries a signed-minimum bias, so raw 0x80000000 is the logical zero. */
constexpr std::uint32_t kSignedZero = 0x80000000;

static_assert(kEncodedSize
              == (kMeaningfulBitCount + encoding::kBitsPerByte - 1) / encoding::kBitsPerByte);
static_assert(kJoinStatusBitOffset + kJoinStatusBitCount <= kMeaningfulBitCount);
static_assert(kPeerHeardWindowBitOffset + kPeerHeardWindowBitCount <= kMeaningfulBitCount);
static_assert(kKeepaliveBitOffset + kKeepaliveBitCount <= kMeaningfulBitCount);
// The two fields abut, so a gap or an overlap here would silently corrupt both.
static_assert(kPeerHeardWindowBitOffset + kPeerHeardWindowBitCount == kKeepaliveBitOffset);
// The status field ends where the host session text begins, and the workspace name ends the body.
static_assert(kJoinStatusBitOffset + kJoinStatusBitCount == kHostSessionTextBitOffset);
static_assert(kOopahReturnCodeBitOffset + kOopahReturnCodeBitCount <= kMeaningfulBitCount);
static_assert(kSpareTextBitOffset + kSpareTextByteCount * encoding::kBitsPerByte
              <= kMeaningfulBitCount);
static_assert(kWorkspaceTextBitOffset + kWorkspaceTextByteCount * encoding::kBitsPerByte
              == kMeaningfulBitCount);

/**
 * Writes one unsigned value into a zeroed most-significant-bit-first field.
 * @param output Whole join-result body, already zeroed.
 * @param bitOffset First destination schema bit.
 * @param bitCount How many low bits of the value to encode.
 */
void write_bits(std::span<std::byte> output,
                std::size_t bitOffset,
                std::size_t bitCount,
                std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < bitCount; ++index) {
        const std::size_t sourceShift = bitCount - index - 1;
        const std::uint32_t sourceMask = std::uint32_t{1} << sourceShift;
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

/**
 * Writes one text into a fixed bias-128 array, terminated and filled to its declared width.
 * @param output Whole join-result body, already zeroed.
 * @param bitOffset First bit of the array's first element.
 * @param byteCount Elements the array declares.
 * @param text Characters stored ahead of the terminator; an empty one is the logical empty string.
 */
void write_text(std::span<std::byte> output,
                std::size_t bitOffset,
                std::size_t byteCount,
                std::string_view text) noexcept {
    for (std::size_t index = 0; index < byteCount; ++index) {
        // The last element is always the terminator, so a text that would fill the array is cut.
        const std::uint32_t character =
            index + 1 < byteCount && index < text.size()
                ? static_cast<std::uint32_t>(static_cast<unsigned char>(text[index]))
                : 0;
        write_bits(output,
                   bitOffset + index * encoding::kBitsPerByte,
                   encoding::kBitsPerByte,
                   (character + kTextNulElement) & 0xFFU);
    }
}

} // namespace

/** Encodes the minimal accepted join result from typed request scalars. */
bool encode_join_result(std::uint32_t correlation,
                        std::uint64_t sessionId,
                        std::uint16_t peerHeardWindowMs,
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
    write_bits(body, kPeerHeardWindowBitOffset, kPeerHeardWindowBitCount, peerHeardWindowMs);
    write_bits(body, kKeepaliveBitOffset, kKeepaliveBitCount, keepaliveHintMs);
    // The biased fields below have no host value yet, so they carry their logical zero. Leaving
    // them at raw zero sends 0x80 text filler and a return code of INT32_MIN.
    write_bits(body, kOopahReturnCodeBitOffset, kOopahReturnCodeBitCount, kSignedZero);
    // The client binds its ActivityClient to the name in this field, so an empty one leaves that
    // bind nameless. It is the same text service 7 publishes for this session in its opaque
    // activity data (server/bap/encrypted/activity_host_manager/activity_establish_response.h),
    // built from the same id, so the establish answer and the join result name one host.
    std::array<char, kHostSessionTextByteCount> hostSession{};
    const int nameLength = std::snprintf(hostSession.data(),
                                         hostSession.size(),
                                         "%08X:%08X@sunrise-activity-host",
                                         static_cast<unsigned>(sessionId >> 32),
                                         static_cast<unsigned>(sessionId & 0xFFFFFFFFULL));
    write_text(body,
               kHostSessionTextBitOffset,
               kHostSessionTextByteCount,
               nameLength > 0 && static_cast<std::size_t>(nameLength) < hostSession.size()
                   ? std::string_view{hostSession.data(), static_cast<std::size_t>(nameLength)}
                   : std::string_view{});
    write_text(body, kSpareTextBitOffset, kSpareTextByteCount, {});
    write_text(body, kWorkspaceTextBitOffset, kWorkspaceTextByteCount, {});
    written = kEncodedSize;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::join_result
