#include <limits>

#include "internal.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
namespace {

/** A presence marker takes 1 schema bit. */
constexpr std::uint8_t kPresenceWidth = 1;
/** Signed-byte fields cannot be wider than the 8-bit host field. */
constexpr std::uint8_t kSignedByteWidth = 8;
/** Optional destination hashes are unsigned 32-bit fields. */
constexpr std::uint8_t kDestinationHashWidth = 32;

} // namespace

/** Reads one optional-field presence bit. */
bool read_presence(encoding::bits::Reader& reader, bool& present) noexcept {
    std::uint64_t encoded = 0;
    if (!reader.read(kPresenceWidth, encoded)) {
        return false;
    }
    present = encoded != 0;
    return true;
}

/** Reads one biased signed-byte schema field with checked narrowing. */
bool read_i8(encoding::bits::Reader& reader,
             std::uint8_t width,
             std::int16_t bias,
             std::int8_t& value) noexcept {
    std::uint64_t encoded = 0;
    if (width > kSignedByteWidth || !reader.read(width, encoded)) {
        return false;
    }
    const std::int64_t decoded = static_cast<std::int64_t>(encoded) - bias;
    if (decoded < (std::numeric_limits<std::int8_t>::min)()
        || decoded > (std::numeric_limits<std::int8_t>::max)()) {
        return false;
    }
    value = static_cast<std::int8_t>(decoded);
    return true;
}

/** Reads one biased signed-short schema field with checked narrowing. */
bool read_i16(encoding::bits::Reader& reader,
              std::uint8_t width,
              std::int16_t bias,
              std::int16_t& value) noexcept {
    std::uint64_t encoded = 0;
    if (width > std::numeric_limits<std::uint16_t>::digits || !reader.read(width, encoded)) {
        return false;
    }
    const std::int64_t decoded = static_cast<std::int64_t>(encoded) - bias;
    if (decoded < (std::numeric_limits<std::int16_t>::min)()
        || decoded > (std::numeric_limits<std::int16_t>::max)()) {
        return false;
    }
    value = static_cast<std::int16_t>(decoded);
    return true;
}

/** Skips one optional opaque field after checking its presence and width. */
bool skip_optional(encoding::bits::Reader& reader, std::size_t width) noexcept {
    bool present = false;
    return read_presence(reader, present) && (!present || reader.skip(width));
}

/** Reads one optional unsigned 32-bit hash. */
bool read_hash(encoding::bits::Reader& reader, bool& present, std::uint32_t& value) noexcept {
    if (!read_presence(reader, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    std::uint64_t encoded = 0;
    if (!reader.read(kDestinationHashWidth, encoded)) {
        return false;
    }
    value = static_cast<std::uint32_t>(encoded);
    return true;
}

} // namespace
  // sunrise::middleware::bap::activity_host_manager::request::selection
