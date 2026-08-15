#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::encoding::bits {

/** Fixed-buffer MSB-first writer for bit-packed Middleware formats. */
class Writer final {
public:
    /** @param output Caller-owned storage cleared before the first bit is written. */
    explicit Writer(std::span<std::byte> output) noexcept;

    /**
     * Builds a writer that only counts bits. Nothing is stored and no width is refused, so an
     * encoder can measure its own body by running once before it writes.
     * @return Writer that accepts every field and reports the total through bit_count.
     */
    [[nodiscard]] static Writer measuring() noexcept;

    /**
     * Writes the low bits of one unsigned value in most-significant-bit-first order.
     * @param value Unsigned source value.
     * @param width Number of low source bits to write, from 0 through 64.
     * @return True when every bit fits in the fixed output buffer.
     */
    [[nodiscard]] bool write(std::uint64_t value, std::uint8_t width) noexcept;

    /**
     * Finishes the zero-padded partial byte.
     * @param written Receives the number of complete or partial output bytes used.
     * @return False when an earlier write exceeded the output buffer.
     */
    [[nodiscard]] bool finish(std::size_t& written) const noexcept;

    /** @return Number of payload bits accepted so far. */
    [[nodiscard]] std::size_t bit_count() const noexcept;

private:
    std::span<std::byte> output_;
    std::size_t bitPosition_{};
    bool failed_{};
    /** A measuring writer stores nothing and never runs out of room. */
    bool measuring_{};
};

} // namespace sunrise::middleware::encoding::bits
