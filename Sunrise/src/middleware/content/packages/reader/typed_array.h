#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "layout.h"

namespace sunrise::middleware::content::packages::reader {

/** Element class argument that accepts any class. */
inline constexpr std::uint32_t kAnyElementClass = 0;

/** Where a checked typed array's rows start, and the class its header names. */
struct TypedArrayRows {
    std::size_t data{};
    std::uint32_t elementClass{};
};

/**
 * Checks one typed-array header: the marker word before it, the repeated count, the element
 * class and that the rows fit after it.
 * @param bytes Whole tag bytes.
 * @param header Offset of the {u64 count, u32 element class, u32 pad} header.
 * @param count Row count the array field holds; the header must repeat it.
 * @param elementClass Class the header must name, or kAnyElementClass.
 * @param stride Row size; zero skips the row bound.
 * @param rows Receives the data offset and the header's element class; untouched on failure.
 * @return True when every check holds.
 */
[[nodiscard]] bool read_typed_array(std::span<const std::byte> bytes,
                                    std::size_t header,
                                    std::uint64_t count,
                                    std::uint32_t elementClass,
                                    std::size_t stride,
                                    TypedArrayRows& rows) noexcept;

} // namespace sunrise::middleware::content::packages::reader
