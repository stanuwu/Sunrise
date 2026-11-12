#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "biased_field.h"
#include "opcode402.h"

namespace sunrise::middleware::web_service::messages::opcode402 {
namespace {

/** The reflected dismantle request occupies exactly 128 bits. */
constexpr std::size_t kPayloadSize = 16;
/** The optional instance identity fills one whole 64-bit field. */
constexpr std::uint8_t kInstanceWidth = 64;
/** Signed native definition indices use one presence bit followed by fifteen value bits. */
constexpr std::uint8_t kDefinitionIndexWidth = 15;
/** The action's own flag. Read to keep the field walk aligned, never gated on. */
constexpr std::uint8_t kRequiredFlagWidth = 1;
/**
 * Bucket ordinal the request was raised from. It counts 0 upward across the character's buckets.
 * The instance identity, or a stack's definition and quantity, already names the item, so the
 * ordinal is read and not acted on.
 */
constexpr std::uint8_t kBucketOrdinalWidth = 7;
/** Pad runs closing the nested descriptor and the whole payload. */
constexpr std::uint8_t kNestedPaddingWidth = 1;
constexpr std::uint8_t kFinalPaddingWidth = 6;

} // namespace

/** Parses the exact native dismantle descriptor of an instance or a stack. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t instancePresent = 0;
    std::uint64_t instanceSoid = 0;
    std::uint64_t definitionPresent = 0;
    std::uint64_t encodedDefinitionIndex = 0;
    std::int32_t quantity = 0;
    std::uint64_t requiredFlag = 0;
    std::uint64_t bucketOrdinal = 0;
    std::uint64_t nestedPadding = 0;
    std::uint64_t finalPadding = 0;
    const bool read =
        message.payload.size() == kPayloadSize && reader.read(1, instancePresent)
        && reader.read(kInstanceWidth, instanceSoid) && reader.read(1, definitionPresent)
        && reader.read(kDefinitionIndexWidth, encodedDefinitionIndex)
        && read_biased_quantity(reader, quantity) && reader.read(kRequiredFlagWidth, requiredFlag)
        && reader.read(kBucketOrdinalWidth, bucketOrdinal)
        && reader.read(kNestedPaddingWidth, nestedPadding)
        && reader.read(kFinalPaddingWidth, finalPadding) && reader.remaining_bits() == 0;

    // Whatever the read reached is kept, so a refused request still describes itself.
    request.instanceSoid = instanceSoid;
    request.definitionIndex = static_cast<std::uint16_t>(encodedDefinitionIndex);
    request.quantity = quantity;

    // The item is already named, so the flag is not gated on. A stack clears both its identity
    // flag and its identity field. A negative quantity is not canonical.
    (void)requiredFlag;
    return read && (instancePresent != 0) == (instanceSoid != 0) && definitionPresent != 0
           && quantity >= 0 && nestedPadding == 0 && finalPadding == 0;
}

} // namespace sunrise::middleware::web_service::messages::opcode402
