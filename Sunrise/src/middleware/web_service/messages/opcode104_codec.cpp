#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode104.h"

namespace sunrise::middleware::web_service::messages::opcode104 {
namespace {

/** The request occupies exactly one byte. */
constexpr std::size_t kPayloadSize = 1;
/** Two absent envelope-blob flags, then four padding bits, all clear. */
constexpr std::uint8_t kTrailerWidth = 6;

} // namespace

/** Parses the two request booleans and checks the clear trailer. */
bool parse_request(const Message& message, Request& output) noexcept {
    output = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t trailer = 0;
    if (!reader.read(1, first) || !reader.read(1, second) || !reader.read(kTrailerWidth, trailer)
        || trailer != 0) {
        return false;
    }
    output = {first != 0, second != 0};
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode104
