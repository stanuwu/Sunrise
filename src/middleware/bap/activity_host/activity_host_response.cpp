#include "activity_host_response.h"

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_host {
namespace {

/** Fixed offsets for the complete svc-16 host request. */
struct RequestLayout {
    /** The big-endian activity-host identity starts the request. */
    static constexpr std::size_t activityHostId = 0;
    /** Svc 16 has no fields after its host identity. */
    static constexpr std::size_t size = activityHostId + encoding::kU64Size;
};

/** Fixed offsets for the complete svc-17 relay response. */
struct ResponseLayout {
    /** The echoed activity-host identity starts the response. */
    static constexpr std::size_t activityHostId = RequestLayout::activityHostId;
    /** The big-endian relay IPv4 address follows the host identity. */
    static constexpr std::size_t relayAddress = activityHostId + encoding::kU64Size;
    /** The neutral protocol field follows the relay address. */
    static constexpr std::size_t neutral = relayAddress + encoding::kU32Size;
    /** The big-endian relay port follows the neutral field. */
    static constexpr std::size_t relayPort = neutral + encoding::kU16Size;
    /** Svc 17 has no fields after its relay port. */
    static constexpr std::size_t size = relayPort + encoding::kU16Size;
};

/** A zero IPv4 address cannot route the activity-host connection. */
constexpr std::uint32_t kInvalidRelayAddress = 0;
/** A zero port cannot route the activity-host connection. */
constexpr std::uint16_t kInvalidRelayPort = 0;
/** Svc 17 needs its middle 16-bit field to stay neutral. */
constexpr std::uint16_t kNeutralField = 0;

} // namespace

/** Checks one svc-16 host identity and encodes its svc-17 relay endpoint. */
bool encode_response(std::span<const std::byte> requestBody,
                     std::uint32_t relayAddress,
                     std::uint16_t relayPort,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (requestBody.size() != RequestLayout::size || relayAddress == kInvalidRelayAddress
        || relayPort == kInvalidRelayPort || output.size() < ResponseLayout::size) {
        return false;
    }

    // Every value is echoed, zero included. The Client drops a reply whose identity does not match
    // the request, and not answering at all would jam its waiting ring instead.
    const std::uint64_t activityHostId = encoding::read_u64_be(
        requestBody.subspan<RequestLayout::activityHostId, encoding::kU64Size>());
    encoding::write_u64_be(output.subspan<ResponseLayout::activityHostId, encoding::kU64Size>(),
                           activityHostId);
    encoding::write_u32_be(output.subspan<ResponseLayout::relayAddress, encoding::kU32Size>(),
                           relayAddress);
    encoding::write_u16_be(output.subspan<ResponseLayout::neutral, encoding::kU16Size>(),
                           kNeutralField);
    encoding::write_u16_be(output.subspan<ResponseLayout::relayPort, encoding::kU16Size>(),
                           relayPort);
    written = ResponseLayout::size;
    return true;
}

} // namespace sunrise::middleware::bap::activity_host
