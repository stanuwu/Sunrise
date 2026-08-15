#include "activity_manager_request.h"

#include "../../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_host_manager::request {
namespace {

/** Fixed fields that precede the service-6 protobuf payload. */
struct RequestLayout final {
    /** The request variant discriminator starts the fixed envelope. */
    static constexpr std::size_t discriminator = 0;
    /** The little-endian protobuf length follows the discriminator byte. */
    static constexpr std::size_t declaredLength = discriminator + sizeof(std::byte);
    /** The protobuf starts after its fixed-width length field. */
    static constexpr std::size_t protobuf = declaredLength + encoding::kU32Size;
    /** The fixed request reserves 7,714 bytes for protobuf and unused padding. */
    static constexpr std::size_t protobufCapacity = 7'714;
    /** Service 6 requires the complete 7,719-byte request body. */
    static constexpr std::size_t size = protobuf + protobufCapacity;
};

/** Discriminator 3 selects the activity-host-manager request variant. */
constexpr std::byte kRequestDiscriminator{3};

} // namespace

/** Checks one service-6 body and borrows only its declared protobuf bytes. */
bool parse_request(std::span<const std::byte> input, Request& request) noexcept {
    request = {};
    if (input.size() != RequestLayout::size
        || input[RequestLayout::discriminator] != kRequestDiscriminator) {
        return false;
    }

    const std::uint32_t declaredLength =
        encoding::read_u32_le(input.subspan<RequestLayout::declaredLength, encoding::kU32Size>());
    if (declaredLength > RequestLayout::protobufCapacity) {
        return false;
    }

    // Only the declared region is borrowed. The padding after it is ignored on purpose.
    request.protobuf =
        input.subspan(RequestLayout::protobuf, static_cast<std::size_t>(declaredLength));
    return true;
}

} // namespace sunrise::middleware::bap::activity_host_manager::request
