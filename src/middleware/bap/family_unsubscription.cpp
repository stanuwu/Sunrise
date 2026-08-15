#include "family_unsubscription.h"

#include "../encoding/byte_order.h"

namespace sunrise::middleware::bap::family_unsubscription {
namespace {

/** The opaque producer flag starts the authenticated request body. */
constexpr std::size_t kFlagOffset = 0;
/** The 8-byte big-endian root id follows the opaque flag. */
constexpr std::size_t kFamilyRootOffset = kFlagOffset + sizeof(std::uint8_t);
/** The smallest svc-14 body ends after the root id. */
constexpr std::size_t kBodySize = kFamilyRootOffset + encoding::kU64Size;

} // namespace

/** Decodes one fixed authenticated family-unsubscription request. */
bool parse(std::span<const std::byte> input, Request& request) noexcept {
    request = {};
    // This size check covers only the two reads below. It is not a rule about the client's body.
    if (input.size() < kBodySize) {
        return false;
    }
    request.flag = std::to_integer<std::uint8_t>(input[kFlagOffset]);
    request.familyRootSoid = encoding::read_u64_be(std::span<const std::byte, encoding::kU64Size>(
        input.data() + kFamilyRootOffset, encoding::kU64Size));
    return true;
}

} // namespace sunrise::middleware::bap::family_unsubscription
