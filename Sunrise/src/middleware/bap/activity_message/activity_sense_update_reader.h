#pragma once

#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"

/** Private to the msg-6 Sense decoder translation units. */
namespace sunrise::middleware::bap::activity_message::sense_update::decoding {
namespace bits = middleware::encoding::bits;

/** Bit reader with its own budget, so one object cannot consume the whole packet. */
class Reader final {
public:
    Reader(bits::Reader& source, std::size_t budget, std::size_t total) noexcept
        : source_(source), left_(budget), total_(total) {}
    [[nodiscard]] bool read(std::uint8_t width, std::uint64_t& value) noexcept {
        if (width > left_ || !source_.read(width, value)) {
            return false;
        }
        left_ -= width;
        return true;
    }
    [[nodiscard]] bool skip(std::size_t width) noexcept {
        if (width > left_ || !source_.skip(width)) {
            return false;
        }
        left_ -= width;
        return true;
    }
    [[nodiscard]] std::size_t left() const noexcept {
        return left_;
    }
    [[nodiscard]] std::size_t position() const noexcept {
        return total_ - source_.remaining_bits();
    }

private:
    bits::Reader& source_;
    std::size_t left_{};
    std::size_t total_{};
};

/** Required fields are present; optional fields consume one presence bit within the budget. */
[[nodiscard]] inline bool present(Reader& reader, bool optional, bool& output) noexcept {
    output = true;
    if (!optional) {
        return true;
    }
    std::uint64_t raw = 0;
    if (!reader.read(1, raw)) {
        return false;
    }
    output = raw != 0;
    return true;
}

/**
 * Consumes the present build86657 80809445 root before the registry group stream.
 * @return False when the root's framing does not fit the reader's budget.
 */
[[nodiscard]] bool consume_root_sense(Reader& reader) noexcept;

} // namespace sunrise::middleware::bap::activity_message::sense_update::decoding
