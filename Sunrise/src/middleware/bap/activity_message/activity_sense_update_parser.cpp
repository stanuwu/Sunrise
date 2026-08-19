/**
 * The sensor sense update. Its first 129 bits are the patch epoch and one literal zero, which is
 * enough to tell whether the client still agrees with the epoch the roster update published. The
 * sense delta behind them has an unresolved width, so the rest of the body stays a bounded tail.
 */

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "sense_update.h"

namespace sunrise::middleware::bap::activity_message::sense_update {

/** Parses a sensor sense update as far as its recovered grammar reaches. */
bool parse_sense_update(std::span<const std::byte> input,
                        SenseUpdate& update,
                        std::size_t& consumedBits) noexcept {
    update = {};
    consumedBits = 0;
    if (input.size() * encoding::kBitsPerByte > kOuterBitCapacity) {
        return false;
    }
    encoding::bits::Reader reader(input);
    std::uint64_t literal = 0;
    if (!reader.read(kEpochFieldWidth, update.epoch.first)
        || !reader.read(kEpochFieldWidth, update.epoch.second)
        || !reader.read(kLiteralZeroWidth, literal)) {
        update = {};
        return false;
    }
    consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
    if (literal != 0) {
        // The bit is a schema literal, so a set bit means this body is not the shape above and no
        // field read from it can be trusted.
        update = {};
        return false;
    }
    update.tailBits = static_cast<std::uint32_t>(reader.remaining_bits());
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::sense_update
