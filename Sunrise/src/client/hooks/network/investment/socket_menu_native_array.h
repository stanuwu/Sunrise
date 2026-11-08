#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../middleware/content/packages/reader/layout.h"
#include "../../../../middleware/content/packages/schema/generated/tag_schema.h"
#include "socket_row_relocation.h"

namespace sunrise::client::hooks::network::investment::native_array {

namespace generated = middleware::content::packages::schema::generated;

/** A native count/self-relative array descriptor. */
using ArrayDescriptor = generated::TagArray<generated::Unknown>;

// Native array ABI: a marker word, then the header, before the first row.
inline constexpr std::size_t kArrayMarkerSize = relocation::kMarkerSize;
inline constexpr std::size_t kArrayHeaderSize = sizeof(relocation::ArrayHeader);

/** One resolved native array: its descriptor, header, first row, count and element class. */
struct ArrayView {
    std::uintptr_t descriptor{};
    std::uintptr_t header{};
    std::uintptr_t data{};
    std::uint64_t count{};
    std::uint32_t elementClass{};
};

/** @return True when both descriptors hold the same count and offset. */
[[nodiscard]] inline bool same(const ArrayDescriptor& left, const ArrayDescriptor& right) noexcept {
    return left.count == right.count && left.relative == right.relative;
}

/** Rebuilds an image pointer without an implementation-defined integer conversion. */
[[nodiscard]] void* image_pointer(std::uintptr_t address) noexcept;

/** @return False when the span is empty or the bytes cannot be read. */
[[nodiscard]] bool read_bytes(std::uintptr_t address, std::span<std::byte> output) noexcept;

template <typename Value> [[nodiscard]] bool read(std::uintptr_t address, Value& value) noexcept {
    return read_bytes(address, std::span(reinterpret_cast<std::byte*>(&value), sizeof value));
}

/** Writes bytes over read-only image data, then puts the page protection back. */
[[nodiscard]] bool write_bytes(std::uintptr_t address, std::span<const std::byte> bytes) noexcept;

template <typename Value>
[[nodiscard]] bool write(std::uintptr_t address, const Value& value) noexcept {
    return write_bytes(address,
                       std::span(reinterpret_cast<const std::byte*>(&value), sizeof value));
}

/** Applies one signed self-relative displacement to a base address. */
[[nodiscard]] bool
add_relative(std::uintptr_t base, std::int64_t relative, std::uintptr_t& output) noexcept;

/** Resolves one native count/self-relative array and validates its repeated header and class. */
[[nodiscard]] bool resolve_array(std::uintptr_t descriptor,
                                 std::size_t maximumCount,
                                 std::size_t stride,
                                 std::uint32_t expectedClass,
                                 ArrayView& output) noexcept;

} // namespace sunrise::client::hooks::network::investment::native_array
