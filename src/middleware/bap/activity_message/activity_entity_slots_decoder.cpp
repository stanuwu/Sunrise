#include <algorithm>

#include "entity_slots.h"

namespace sunrise::middleware::bap::activity_message::entity_slots {

/** Decodes the fixed entity-slot prefix without changing its wire byte order. */
bool decode_entity_slots(std::span<const std::byte> input, EntitySlotMask& mask) noexcept {
    mask = {};
    // The bound is only what the reads below need. An exact length is the client's
    // business, not a rule to enforce here.
    if (input.size() < kEncodedSize) {
        return false;
    }

    std::copy_n(input.begin(), kEncodedSize, mask.begin());
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::entity_slots
