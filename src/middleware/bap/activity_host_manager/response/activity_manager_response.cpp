#include "activity_manager_response.h"

#include <algorithm>
#include <array>

#include "../../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_host_manager::response {
namespace {

/** The fixed service-7 tail reserves 128 opaque activity-data bytes. */
constexpr std::size_t kActivityDataSize = 128;

/** Complete typed service-7 body before it is copied to caller-owned storage. */
struct ResponseBody final {
    /** Discriminator 2 selects the activity-host-manager response variant. */
    std::byte discriminator;
    /** The client reads the session id as one big-endian u64. */
    std::array<std::byte, encoding::kU64Size> sessionId;
    /** Our minimal policy leaves all 128 opaque activity-data bytes zero. */
    std::array<std::byte, kActivityDataSize> activityData;
};

/** Service 7 rejects a response body with discriminator zero or any value other than 2. */
constexpr std::byte kResponseDiscriminator{2};
/** A zero session id does not move the activity-host request on. */
constexpr std::uint64_t kInvalidSessionId = 0;
/** A failed encode publishes no response bytes. */
constexpr std::size_t kNoBytesWritten = 0;
/** Service 7 needs one discriminator, one u64 and 128 opaque bytes. */
constexpr std::size_t kResponseBodySize = 137;

static_assert(sizeof(ResponseBody) == kResponseBodySize);

} // namespace

/** Encodes a service-7 response with the minimal local activity-data policy. */
bool encode_response(std::uint64_t sessionId,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = kNoBytesWritten;
    if (sessionId == kInvalidSessionId || output.size() < kResponseBodySize) {
        return false;
    }

    ResponseBody body{};
    body.discriminator = kResponseDiscriminator;
    encoding::write_u64_be(body.sessionId, sessionId);

    const auto bytes = std::as_bytes(std::span{&body, 1});
    std::copy(bytes.begin(), bytes.end(), output.begin());
    written = bytes.size();
    return true;
}

} // namespace sunrise::middleware::bap::activity_host_manager::response
