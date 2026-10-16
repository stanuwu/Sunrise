#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "field_reader.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * Finds the offset of one fixed-stride array element.
 * @param dataOffset Array data offset.
 * @param count Array element count.
 * @param stride Element stride.
 * @param index Element ordinal.
 * @param offset Receives the element offset.
 * @return True when the ordinal is inside the array.
 */
[[nodiscard]] inline bool element_offset(std::size_t dataOffset,
                                         std::uint64_t count,
                                         std::size_t stride,
                                         std::uint64_t index,
                                         std::size_t& offset) noexcept {
    if (index >= count) {
        return false;
    }
    offset = dataOffset + static_cast<std::size_t>(index) * stride;
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
