#include "../../encoding/bit_reader.h"
#include "opcode504.h"

namespace sunrise::middleware::web_service::messages::opcode504 {
namespace {

/** The whole request is one unaligned 64-bit id, with no selector before it. */
constexpr std::uint8_t kCharacterSoidWidth = 64;
/** 64 meaningful bits need 8 bytes. */
constexpr std::size_t kMinimumPayloadSize = 8;

} // namespace

/** Parses the bare 64-bit picked-character id. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() < kMinimumPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    return reader.read(kCharacterSoidWidth, request.characterSoid);
}

} // namespace sunrise::middleware::web_service::messages::opcode504
