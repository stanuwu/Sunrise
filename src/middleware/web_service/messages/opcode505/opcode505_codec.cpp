#include "opcode505_codec.h"

#include <algorithm>
#include <array>

namespace sunrise::middleware::web_service::messages::opcode505 {

/** Reports whether this request is the change-character transition. */
bool parse_request(const Message& message) noexcept {
    return message.opcode == kOpcode;
}

/** Encodes the successful response with the family-4 version to wait for. */
bool encode_response(const Message& message,
                     std::int32_t nextVersion,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (!parse_request(message) || output.size() < kResponseSize) {
        return false;
    }

    std::array<std::byte, kResponseSize> staged{};
    StatusResponse status{};
    status.value = nextVersion;
    std::size_t stagedSize = 0;
    if (!sunrise::middleware::web_service::encode_response(
            message, ResponseShape::statusPair, status, staged, stagedSize)) {
        return false;
    }

    std::copy_n(staged.begin(), stagedSize, output.begin());
    written = stagedSize;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode505
