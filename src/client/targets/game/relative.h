#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "../../patterns/registry.h"

namespace sunrise::client::targets::game::relative {

/**
 * Tests whether one bounded address range belongs to a scanned executable range.
 * @param image Valid executable image ranges.
 * @param address First address to test.
 * @param size Required contiguous byte count.
 * @return True when every requested byte belongs to one range.
 */
[[nodiscard]] inline bool contains(std::span<const patterns::ImageRange> image,
                                   const std::byte* address,
                                   std::size_t size = 1) noexcept {
    if (address == nullptr || size == 0) {
        return false;
    }
    const auto candidate = reinterpret_cast<std::uintptr_t>(address);
    for (const patterns::ImageRange range : image) {
        const auto begin = reinterpret_cast<std::uintptr_t>(range.bytes.data());
        if (candidate >= begin) {
            const auto offset = candidate - begin;
            if (offset < range.bytes.size() && size <= range.bytes.size() - offset) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Resolves one signed instruction-relative address without pointer overflow.
 * @param instruction Matched instruction sequence base.
 * @param displacementOffset Signed displacement byte offset.
 * @param endOffset Offset immediately after the instruction.
 * @param target Receives the absolute target address.
 * @return True when the signed address calculation is representable.
 */
[[nodiscard]] inline bool resolve(std::byte* instruction,
                                  std::size_t displacementOffset,
                                  std::size_t endOffset,
                                  std::byte*& target) noexcept {
    target = nullptr;
    if (instruction == nullptr) {
        return false;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + displacementOffset, sizeof displacement);
    const auto base = reinterpret_cast<std::uintptr_t>(instruction);
    if (endOffset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    const auto end = base + endOffset;
    if (displacement < 0) {
        const auto magnitude =
            static_cast<std::uint32_t>(-(static_cast<std::int64_t>(displacement)));
        if (magnitude > end) {
            return false;
        }
        target = std::bit_cast<std::byte*>(end - magnitude);
        return true;
    }
    const auto magnitude = static_cast<std::uint32_t>(displacement);
    if (magnitude > (std::numeric_limits<std::uintptr_t>::max)() - end) {
        return false;
    }
    target = std::bit_cast<std::byte*>(end + magnitude);
    return true;
}

} // namespace sunrise::client::targets::game::relative
