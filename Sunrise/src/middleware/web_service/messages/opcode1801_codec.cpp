#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode1801.h"

namespace sunrise::middleware::web_service::messages::opcode1801 {
namespace {

/** The reflected record claim request occupies exactly 24 bits. */
constexpr std::size_t kPayloadSize = 3;
/** The optional record carries one presence bit before its row. */
constexpr std::uint8_t kPresenceWidth = 1;
/** Native record rows are addressed by a fifteen-bit index. */
constexpr std::uint8_t kRecordIndexWidth = 15;
/** The descriptor pads its two payload bytes out to three. */
constexpr std::uint8_t kPaddingWidth = 8;

} // namespace

/** Parses the exact native record claim descriptor. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t present = 0;
    std::uint64_t encodedRecordIndex = 0;
    std::uint64_t padding = 0;
    if (!reader.read(kPresenceWidth, present) || !reader.read(kRecordIndexWidth, encodedRecordIndex)
        || !reader.read(kPaddingWidth, padding) || reader.remaining_bits() != 0 || present == 0
        || padding != 0) {
        return false;
    }
    request.recordIndex = static_cast<std::uint16_t>(encodedRecordIndex);
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode1801
