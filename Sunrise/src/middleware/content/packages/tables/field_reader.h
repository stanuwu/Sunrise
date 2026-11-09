#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "../reader/container_framing.h"

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
 * Resolves a self-relative field.
 * @param field Offset of the 8-byte signed delta.
 * @param target Receives the resolved offset.
 * @return False when the delta is zero or the target falls outside the blob.
 */
[[nodiscard]] inline bool
relative(std::span<const std::byte> bytes, std::size_t field, std::size_t& target) noexcept {
    std::int64_t delta{};
    if (!read(bytes, field, delta) || delta == 0
        || field > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    const auto base = static_cast<std::int64_t>(field);
    if (delta < -base || delta > (std::numeric_limits<std::int64_t>::max)() - base) {
        return false;
    }
    target = static_cast<std::size_t>(base + delta);
    return target < bytes.size();
}

/**
 * Adds a signed self-relative offset to its field's position without overflow.
 * @param base Offset of the field that holds the relative value.
 * @param relative Signed distance from that field.
 * @param output Receives the target offset; unchanged on failure.
 * @return False when the sum overflows or is negative.
 */
[[nodiscard]] inline bool
relative_offset(std::size_t base, std::int64_t relative, std::size_t& output) noexcept {
    // Signed range the addition must stay inside.
    constexpr std::int64_t kMaximum = (std::numeric_limits<std::int64_t>::max)();
    constexpr std::int64_t kMinimum = (std::numeric_limits<std::int64_t>::min)();
    if (base > static_cast<std::size_t>(kMaximum)) {
        return false;
    }
    const auto signedBase = static_cast<std::int64_t>(base);
    if ((relative > 0 && signedBase > kMaximum - relative)
        || (relative < 0 && signedBase < kMinimum - relative)) {
        return false;
    }
    const std::int64_t target = signedBase + relative;
    if (target < 0) {
        return false;
    }
    output = static_cast<std::size_t>(target);
    return true;
}
/**
 * Reads float lanes without imposing alignment on package bytes.
 * @param output Receives the lanes; zeroed on failure.
 * @return False when a lane lies outside the blob.
 */
template <std::size_t Size>
[[nodiscard]] bool read_vector(std::span<const std::byte> blob,
                               std::size_t offset,
                               std::array<float, Size>& output) noexcept {
    output = {};
    for (std::size_t lane = 0; lane < Size; ++lane) {
        if (!read(blob, offset + lane * sizeof(float), output[lane])) {
            output = {};
            return false;
        }
    }
    return true;
}

/**
 * Reads one serialized boolean.
 * @return False when the byte is outside the blob or holds anything but zero or one.
 */
[[nodiscard]] inline bool
read_boolean(std::span<const std::byte> blob, std::size_t offset, bool& value) noexcept {
    std::uint8_t raw = 0;
    if (!read(blob, offset, raw) || raw > 1) {
        return false;
    }
    value = raw != 0;
    return true;
}

/** A serialized block's 32-bit class ID sits immediately before its payload. */
inline constexpr std::size_t kBlockClassPrefixSize = reader::layout::kClassWordSize;

/**
 * Locates a class-tagged block through its self-relative field. A zero field names no block.
 * @param present Receives whether the field names a block.
 * @param block Receives the payload offset, which is meaningful only when present is set.
 * @return False when the field is unreadable or names an out-of-bounds or wrong-class block.
 */
[[nodiscard]] inline bool class_block(std::span<const std::byte> bytes,
                                      std::size_t field,
                                      std::uint32_t expectedClass,
                                      std::size_t& block,
                                      bool& present) noexcept {
    present = false;
    std::int64_t delta{};
    if (!read(bytes, field, delta)) {
        return false;
    }
    if (delta == 0) {
        return true;
    }
    std::uint32_t actualClass{};
    if (!relative(bytes, field, block) || block < kBlockClassPrefixSize
        || !read(bytes, block - kBlockClassPrefixSize, actualClass)
        || actualClass != expectedClass) {
        return false;
    }
    present = true;
    return true;
}
} // namespace sunrise::middleware::content::packages::tables
