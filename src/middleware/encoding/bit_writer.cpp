#include "bit_writer.h"

#include <algorithm>
#include <climits>

namespace sunrise::middleware::encoding::bits {
namespace {

/** Writer values are limited to the width of the unsigned 64-bit source type. */
constexpr std::uint8_t kMaximumWriteWidth = 64;
/** Windows wire formats require 8-bit bytes. */
constexpr std::size_t kBitsPerByte = CHAR_BIT;
static_assert(kBitsPerByte == 8);

} // namespace

Writer::Writer(std::span<std::byte> output) noexcept : output_(output) {
    std::fill(output_.begin(), output_.end(), std::byte{});
}

/** Builds a writer that only counts bits. */
Writer Writer::measuring() noexcept {
    Writer writer{{}};
    writer.measuring_ = true;
    return writer;
}

/** Writes one bounded unsigned field without allocation or byte alignment. */
bool Writer::write(std::uint64_t value, std::uint8_t width) noexcept {
    const std::size_t capacity = output_.size() * kBitsPerByte;
    if (failed_ || width > kMaximumWriteWidth || (!measuring_ && width > capacity - bitPosition_)) {
        failed_ = true;
        return false;
    }
    if (measuring_) {
        bitPosition_ += width;
        return true;
    }
    for (std::uint8_t index = 0; index < width; ++index) {
        const std::uint8_t sourceShift = static_cast<std::uint8_t>(width - index - 1);
        const std::uint64_t bit = (value >> sourceShift) & 1U;
        const std::size_t byteIndex = bitPosition_ / kBitsPerByte;
        const std::size_t destinationShift = kBitsPerByte - 1 - (bitPosition_ % kBitsPerByte);
        if (bit != 0) {
            output_[byteIndex] |= static_cast<std::byte>(1U << destinationShift);
        }
        ++bitPosition_;
    }
    return true;
}

/** Reports the fixed output prefix containing all accepted bits. */
bool Writer::finish(std::size_t& written) const noexcept {
    written = 0;
    if (failed_) {
        return false;
    }
    written = bitPosition_ / kBitsPerByte;
    if (bitPosition_ % kBitsPerByte != 0) {
        ++written;
    }
    return true;
}

/** @return Number of payload bits accepted so far. */
std::size_t Writer::bit_count() const noexcept {
    return bitPosition_;
}

} // namespace sunrise::middleware::encoding::bits
