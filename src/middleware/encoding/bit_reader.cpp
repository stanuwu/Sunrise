#include "bit_reader.h"

#include <limits>

#include "byte_order.h"

namespace sunrise::middleware::encoding::bits {
namespace {

/** Reader fields are bounded by the unsigned 64-bit destination type. */
constexpr std::uint8_t kMaximumReadWidth = 64;

} // namespace

Reader::Reader(std::span<const std::byte> input) noexcept : input_(input) {}

/** Checks one requested width and makes failure sticky without moving the cursor. */
bool Reader::can_consume(std::size_t width) noexcept {
    // Check the byte-to-bit multiply before forming the bit size, then check the cursor before
    // subtracting it. This order keeps both size steps bounded for any caller value.
    const std::size_t maximumByteCount =
        (std::numeric_limits<std::size_t>::max)() / encoding::kBitsPerByte;
    if (failed_ || input_.size() > maximumByteCount) {
        failed_ = true;
        return false;
    }
    const std::size_t capacity = input_.size() * encoding::kBitsPerByte;
    if (bitPosition_ > capacity || width > capacity - bitPosition_) {
        failed_ = true;
        return false;
    }
    return true;
}

/** Reads one bounded unsigned field without allocation or byte alignment. */
bool Reader::read(std::uint8_t width, std::uint64_t& value) noexcept {
    value = 0;
    if (width > kMaximumReadWidth) {
        failed_ = true;
        return false;
    }
    if (!can_consume(width)) {
        return false;
    }
    for (std::uint8_t index = 0; index < width; ++index) {
        const std::size_t byteIndex = bitPosition_ / encoding::kBitsPerByte;
        const std::size_t sourceShift =
            encoding::kBitsPerByte - 1 - (bitPosition_ % encoding::kBitsPerByte);
        const std::uint64_t bit =
            (std::to_integer<std::uint64_t>(input_[byteIndex]) >> sourceShift) & 1U;
        value = (value << 1U) | bit;
        ++bitPosition_;
    }
    return true;
}

/** Advances over one bounded field without assembling its bits into a scalar. */
bool Reader::skip(std::size_t width) noexcept {
    if (!can_consume(width)) {
        return false;
    }
    bitPosition_ += width;
    return true;
}

/** @return Number of unread input bits after successful fields. */
std::size_t Reader::remaining_bits() const noexcept {
    if (input_.size() > (std::numeric_limits<std::size_t>::max)() / encoding::kBitsPerByte) {
        return 0;
    }
    return input_.size() * encoding::kBitsPerByte - bitPosition_;
}

} // namespace sunrise::middleware::encoding::bits
