#include "internal.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
namespace {

/** Bits in one byte. Both the source and the output cursor step by this. */
constexpr std::size_t kBitsPerByte = 8;
/** The most significant bit of a byte, where every bit-packed field starts. */
constexpr unsigned kHighBit = 0x80;

} // namespace

/** Copies one bit range out of a bit-packed buffer, left-aligned into byte storage. */
bool copy_bits(std::span<const std::byte> source,
               std::size_t startBit,
               std::size_t bitCount,
               std::span<std::byte> output,
               std::size_t& outputBitLength) noexcept {
    outputBitLength = 0;
    if (startBit + bitCount > source.size() * kBitsPerByte
        || bitCount > output.size() * kBitsPerByte) {
        return false;
    }
    for (std::size_t index = 0; index < bitCount; ++index) {
        const std::size_t sourceBit = startBit + index;
        const unsigned bit = static_cast<unsigned>(source[sourceBit / kBitsPerByte])
                                 >> (kBitsPerByte - 1 - sourceBit % kBitsPerByte)
                             & 1U;
        if (bit == 0) {
            continue;
        }
        std::byte& target = output[index / kBitsPerByte];
        target = static_cast<std::byte>(static_cast<unsigned>(target)
                                        | kHighBit >> (index % kBitsPerByte));
    }
    outputBitLength = bitCount;
    return true;
}

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
