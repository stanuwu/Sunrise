#include <cstddef>

#include "../../../state/account/inventory/item_state.h"
#include "../../encoding/bit_reader.h"
#include "opcode406.h"

namespace sunrise::middleware::web_service::messages::opcode406 {
namespace {

/** The reflected item-state request occupies exactly 120 bits. */
constexpr std::size_t kPayloadSize = 15;
/** Signed native definition indices use one presence bit followed by fifteen value bits. */
constexpr std::uint8_t kDefinitionIndexWidth = 15;
/** The accumulated state value fills one signed 32-bit field. */
constexpr std::uint8_t kValueWidth = 32;
/** The descriptor pads its fields out to whole bytes. */
constexpr std::uint8_t kPaddingWidth = 7;
/** Nonnegative signed 32-bit values have this bit set after native descriptor biasing. */
constexpr std::uint64_t kValueBias = 0x80000000ULL;

} // namespace

/** Parses the exact native item-state descriptor. */
bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t instancePresent = 0;
    std::uint64_t instanceSoid = 0;
    std::uint64_t definitionPresent = 0;
    std::uint64_t definitionIndex = 0;
    std::uint64_t encodedFlags = 0;
    std::uint64_t padding = 0;
    const bool read = message.payload.size() == kPayloadSize && reader.read(1, instancePresent)
                      && reader.read(64, instanceSoid) && reader.read(1, definitionPresent)
                      && reader.read(kDefinitionIndexWidth, definitionIndex)
                      && reader.read(kValueWidth, encodedFlags)
                      && reader.read(kPaddingWidth, padding) && reader.remaining_bits() == 0;

    // Whatever the read reached is kept, so a refused request still describes itself.
    request.instanceSoid = instanceSoid;
    request.definitionIndex = static_cast<std::uint16_t>(definitionIndex);
    if (encodedFlags >= kValueBias) {
        request.flags = static_cast<std::uint32_t>(encodedFlags - kValueBias);
    }

    if (!read || instancePresent == 0 || instanceSoid == 0 || definitionPresent == 0
        || encodedFlags < kValueBias || padding != 0
        || !state::account::inventory::valid_item_state(
            static_cast<std::uint32_t>(encodedFlags - kValueBias))) {
        return false;
    }
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode406
