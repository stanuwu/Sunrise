#include "../../encoding/bit_reader.h"
#include "opcode206.h"

namespace sunrise::middleware::web_service::messages::opcode206 {
namespace {

/** Family type is 4 bits and stores logical values with a bias of 1. */
constexpr std::uint8_t kFamilyTypeWidth = 4;
/** Family type descriptor adds 1 before writing the 4-bit field. */
constexpr std::uint64_t kFamilyTypeBias = 1;
/** Family root id is the next 64 bits, not byte aligned. */
constexpr std::uint8_t kFamilyRootWidth = 64;
/** 68 meaningful bits need 9 bytes, including the zero padding. */
constexpr std::size_t kMinimumPayloadSize = 9;

} // namespace

/** Parses the 4-bit type and the unaligned 64-bit family root. */
bool parse_request(const Message& message, queuez::Subscription& subscription) noexcept {
    subscription = {};
    if (message.opcode != kOpcode || message.payload.size() < kMinimumPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t storedFamily = 0;
    if (!reader.read(kFamilyTypeWidth, storedFamily)
        || !reader.read(kFamilyRootWidth, subscription.familyRootSoid)) {
        subscription = {};
        return false;
    }
    subscription.familyType = static_cast<std::uint32_t>(storedFamily - kFamilyTypeBias);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode206
