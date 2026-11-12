#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode2002.h"

namespace sunrise::middleware::web_service::messages::opcode2002 {
namespace {

/** The reflected opening request occupies exactly 72 bits. */
constexpr std::size_t kPayloadSize = 9;
/** The instance identity fills one whole 64-bit field. */
constexpr std::uint8_t kInstanceWidth = 64;
/** Two absent envelope fields and six alignment bits close the payload. */
constexpr std::uint8_t kFinalPaddingWidth = 8;

} // namespace

/** Reads the instance and rejects a nonzero closing run. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t finalPadding = 0;
    if (!reader.read(kInstanceWidth, request.instanceSoid)
        || !reader.read(kFinalPaddingWidth, finalPadding) || reader.remaining_bits() != 0
        || finalPadding != 0) {
        request = {};
        return false;
    }
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode2002
