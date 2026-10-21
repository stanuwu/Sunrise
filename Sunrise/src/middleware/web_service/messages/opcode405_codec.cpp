#include <cstddef>
#include <limits>

#include "../../encoding/bit_reader.h"
#include "biased_field.h"
#include "opcode405.h"

namespace sunrise::middleware::web_service::messages::opcode405 {
namespace {

/** The vendor transfer request occupies exactly 144 bits. */
constexpr std::size_t kPayloadSize = 18;
/** The source bucket is one signed byte biased by its midpoint. */
constexpr std::uint8_t kSourceBucketWidth = 8;
constexpr std::int32_t kSourceBucketBias = -(std::numeric_limits<std::int8_t>::min)();
/** The instance identity fills one whole 64-bit field. */
constexpr std::uint8_t kInstanceWidth = 64;
/** The quantity is one signed 32-bit field biased by its midpoint. */
constexpr std::uint8_t kQuantityWidth = 32;
constexpr std::int64_t kQuantityBias =
    -static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)());
/** Two absent envelope fields and six alignment bits close the payload. */
constexpr std::uint8_t kFinalPaddingWidth = 8;

} // namespace

/** Reads the biased signed fields and rejects a nonzero closing pad run. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t sourceBucket = 0;
    std::uint64_t quantity = 0;
    std::uint64_t finalPadding = 0;
    if (!read_biased_index(reader, request.vendorIndex)
        || !reader.read(kSourceBucketWidth, sourceBucket)
        || !reader.read(kInstanceWidth, request.instanceSoid)
        || !read_biased_index(reader, request.definitionIndex)
        || !reader.read(kQuantityWidth, quantity) || !reader.read(kFinalPaddingWidth, finalPadding)
        || reader.remaining_bits() != 0 || finalPadding != 0) {
        request = {};
        return false;
    }
    request.sourceBucket =
        static_cast<std::int8_t>(static_cast<std::int32_t>(sourceBucket) - kSourceBucketBias);
    request.quantity =
        static_cast<std::int32_t>(static_cast<std::int64_t>(quantity) - kQuantityBias);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode405
