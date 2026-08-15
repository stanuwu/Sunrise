#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace sunrise::middleware::content::packages::tables {

/**
 * Reads one little-endian field that must lie inside the blob.
 * @param blob Whole definition bytes.
 * @param offset Field offset.
 * @param value Receives the field.
 * @return True when the whole field is inside the blob.
 */
template <typename Value>
[[nodiscard]] inline bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    // Subtracting rather than adding keeps a large offset from wrapping past the size.
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

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
