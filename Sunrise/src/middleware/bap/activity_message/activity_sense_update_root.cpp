#include "activity_sense_update_reader.h"

namespace sunrise::middleware::bap::activity_message::sense_update::decoding {
namespace {

/** Consumes one required count and its fixed-width elements, bounded by the native descriptor. */
[[nodiscard]] bool consume_root_array(Reader& reader,
                                      std::uint8_t countWidth,
                                      std::uint32_t capacity,
                                      std::uint8_t elementWidth) noexcept {
    std::uint64_t count = 0;
    return reader.read(countWidth, count) && count <= capacity
           && reader.skip(static_cast<std::size_t>(count) * elementWidth);
}

/**
 * Build86657 schemas 8080944C/80809452: optional word list, fixed words, and byte list.
 * Only their framing is consumed; these root channels do not become squad observations.
 */
[[nodiscard]] bool consume_root_channels(Reader& reader,
                                         std::uint8_t countWidth,
                                         std::uint32_t capacity,
                                         std::uint32_t fixedWords) noexcept {
    bool exists = false;
    if (!present(reader, true, exists)
        || (exists && !consume_root_array(reader, countWidth, capacity, 32))) {
        return false;
    }
    if (!present(reader, true, exists) || (exists && !reader.skip(fixedWords * 32))) {
        return false;
    }
    return present(reader, true, exists)
           && (!exists || consume_root_array(reader, countWidth, capacity, 8));
}

} // namespace

/**
 * Consumes the present build86657 80809445 root before the registry group stream.
 * Its global channels hold 256 entries; up to 64 keyed rows each hold 96 local entries.
 * Counts and nested presence come from the exact native reflection descriptors.
 */
bool consume_root_sense(Reader& reader) noexcept {
    constexpr std::uint32_t globalCapacity = 256, keyedCapacity = 64, localCapacity = 96;
    bool exists = false;
    if (!present(reader, true, exists)
        || (exists && !consume_root_channels(reader, 9, globalCapacity, 8))) {
        return false;
    }
    if (!present(reader, true, exists)) {
        return false;
    }
    if (!exists) {
        return true;
    }
    std::uint64_t count = 0;
    if (!reader.read(7, count) || count > keyedCapacity) {
        return false;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!present(reader, true, exists) || (exists && !reader.skip(32))) {
            return false;
        }
        if (!present(reader, true, exists)
            || (exists && !consume_root_channels(reader, 7, localCapacity, 3))) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::sense_update::decoding
