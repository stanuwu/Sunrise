#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::encoding::bits {

/** Fixed-buffer MSB-first reader for bit-packed Middleware formats. */
class Reader final {
public:
    /** @param input Complete immutable bit-packed storage. */
    explicit Reader(std::span<const std::byte> input) noexcept;

    /**
     * Reads one unsigned field in most-significant-bit-first order.
     * @param width Number of bits to consume, from 0 through 64.
     * @param value Receives the zero-extended field value.
     * @return True when the complete field is available.
     */
    [[nodiscard]] bool read(std::uint8_t width, std::uint64_t& value) noexcept;

    /**
     * Advances over a bounded field without assembling its value.
     * A failed bound check marks the reader failed and leaves the cursor unchanged.
     * @param width Number of bits to consume.
     * @return True when the complete field was available and consumed.
     */
    [[nodiscard]] bool skip(std::size_t width) noexcept;

    /** @return Number of unread input bits after successful fields. */
    [[nodiscard]] std::size_t remaining_bits() const noexcept;

private:
    /** @return True when width can be consumed without changing the cursor. */
    [[nodiscard]] bool can_consume(std::size_t width) noexcept;

    std::span<const std::byte> input_;
    std::size_t bitPosition_{};
    bool failed_{};
};

} // namespace sunrise::middleware::encoding::bits
