#include <algorithm>

#include "entity_slots.h"

namespace sunrise::middleware::bap::activity_message::entity_slots {

/** Encodes one selected entity-slot lease mask without changing its wire byte order. */
bool encode_entity_slots(std::span<const std::byte, kEncodedSize> mask,
                         std::span<std::byte> output,
                         std::size_t& written) noexcept {
    written = {};
    if (output.size() < kEncodedSize) {
        return false;
    }

    std::copy(mask.begin(), mask.end(), output.begin());
    written = kEncodedSize;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::entity_slots
