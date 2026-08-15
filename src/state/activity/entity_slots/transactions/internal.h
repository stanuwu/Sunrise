#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "../../definition.h"

namespace sunrise::state::activity::entity_slots::transactions {

/** @return True when no lease bit is set. */
inline bool empty(const LeaseMask& mask) noexcept {
    for (const std::byte value : mask) {
        if (value != std::byte{}) {
            return false;
        }
    }
    return true;
}

/**
 * Picks free logical slots in ascending order.
 * @param held Slots the new grant cannot use.
 * @param requestedCount Most slots to pick.
 * @return Mask with at most requestedCount free bits set.
 */
inline LeaseMask select_free(const LeaseMask& held, std::size_t requestedCount) noexcept {
    LeaseMask selected{};
    const std::size_t wanted = (std::min)(requestedCount, kSlotCount);
    std::size_t count = 0;
    for (std::size_t slot = 0; slot < kSlotCount && count < wanted; ++slot) {
        const std::size_t byteIndex = slot / kSlotsPerByte;
        const std::size_t bitIndex = slot % kSlotsPerByte;
        const std::byte bit{static_cast<unsigned char>(1U << bitIndex)};
        if ((held[byteIndex] & bit) != std::byte{}) {
            continue;
        }
        selected[byteIndex] |= bit;
        ++count;
    }
    return selected;
}

/** @return Bits set in both lease masks. */
inline LeaseMask intersect(const LeaseMask& first, const LeaseMask& second) noexcept {
    LeaseMask result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = first[index] & second[index];
    }
    return result;
}

/** @return True when both masks hold the same bits. */
inline bool equal(const LeaseMask& first, const LeaseMask& second) noexcept {
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (first[index] != second[index]) {
            return false;
        }
    }
    return true;
}

/** @return True when selected contains a bit absent from available. */
inline bool exceeds(const LeaseMask& selected, const LeaseMask& available) noexcept {
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if ((selected[index] & ~available[index]) != std::byte{}) {
            return true;
        }
    }
    return false;
}

} // namespace sunrise::state::activity::entity_slots::transactions
