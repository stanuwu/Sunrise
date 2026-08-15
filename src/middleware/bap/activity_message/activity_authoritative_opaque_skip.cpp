#include "client_authoritative_data.h"

namespace sunrise::middleware::bap::activity_message::client_authoritative_data {
namespace {

/** The B2 byte-list count uses 14 bits and allows at most 10,000 elements. */
constexpr std::uint8_t kLargeArrayCountWidth = 14;
/** The B2 byte-list schema holds 10,000 elements. */
constexpr std::uint64_t kLargeArrayMaximumCount = 10'000;
/** The D9 byte-list count uses 8 bits and allows at most 128 elements. */
constexpr std::uint8_t kSmallArrayCountWidth = 8;
/** The D9 byte-list schema holds 128 elements. */
constexpr std::uint64_t kSmallArrayMaximumCount = 128;
/** Every dynamic element in both opaque branches is one byte. */
constexpr std::size_t kArrayElementBitCount = 8;

/**
 * Skips one optional fixed-width field.
 * @param reader Reader sitting at the field marker.
 * @param width Field width when the field is present.
 * @return True when the marker and any present value fit.
 */
[[nodiscard]] bool skip_optional(encoding::bits::Reader& reader, std::size_t width) noexcept {
    bool present = false;
    return read_presence(reader, present) && (!present || reader.skip(width));
}

/**
 * Skips one D9 record, including its dynamic byte list.
 * @param reader Reader sitting at the first required D9 field.
 * @return True when the count is valid and the whole record fits.
 */
[[nodiscard]] bool skip_d9(encoding::bits::Reader& reader) noexcept {
    std::uint64_t count = 0;
    return reader.skip(8) && reader.skip(2) && reader.read(kSmallArrayCountWidth, count)
           && count <= kSmallArrayMaximumCount
           && reader.skip(static_cast<std::size_t>(count) * kArrayElementBitCount)
           && reader.skip(64);
}

/** The leg's first scalar is a 10-bit region index at bias 1. */
constexpr std::uint8_t kRegionIndexWidth = 10;
/** The leg's second scalar is that region's 32-bit name hash. */
constexpr std::uint8_t kRegionHashWidth = 32;
/** Logical signed fields subtract 1 from their unsigned wire value. */
constexpr std::int32_t kSignedFieldBias = 1;

/**
 * Reads one complete D6 leg, keeping its region scalars only when asked. Both legs have the same
 * shape and only the second names the player's current region, so the first is walked and
 * nothing is kept.
 * @param reader Reader sitting at the first required D6 scalar.
 * @param region Receives the region when keepRegion is set.
 * @param keepRegion True for the leg that carries the player's region.
 * @return True when every required scalar and optional child fits.
 */
[[nodiscard]] bool
read_d6(encoding::bits::Reader& reader, RegionState& region, bool keepRegion) noexcept {
    std::uint64_t indexWire = 0;
    std::uint64_t hash = 0;
    if (!reader.read(kRegionIndexWidth, indexWire) || !reader.read(kRegionHashWidth, hash)
        || !reader.skip(32) || !reader.skip(2) || !reader.skip(2) || !skip_optional(reader, 8)) {
        return false;
    }
    if (keepRegion) {
        region.index = static_cast<std::int32_t>(indexWire) - kSignedFieldBias;
        region.hash = static_cast<std::uint32_t>(hash);
        region.hasHash = true;
    }
    bool d9Present = false;
    return read_presence(reader, d9Present) && (!d9Present || skip_d9(reader));
}

/**
 * Skips the optional dynamic byte-list field inside B2.
 * @param reader Reader sitting at the B4 presence marker.
 * @return True when the marker is absent, or the whole list is there and within its limit.
 */
[[nodiscard]] bool skip_large_array(encoding::bits::Reader& reader) noexcept {
    bool present = false;
    std::uint64_t count = 0;
    if (!read_presence(reader, present)) {
        return false;
    }
    if (!present) {
        return true;
    }
    return reader.read(kLargeArrayCountWidth, count) && count <= kLargeArrayMaximumCount
           && reader.skip(static_cast<std::size_t>(count) * kArrayElementBitCount);
}

} // namespace

bool read_presence(encoding::bits::Reader& reader, bool& present) noexcept {
    present = false;
    std::uint64_t wire = 0;
    if (!reader.read(1, wire)) {
        return false;
    }
    present = wire != 0;
    return true;
}

/** Skips the whole opaque B2 branch and checks its dynamic count. */
bool skip_opaque_root_branch(encoding::bits::Reader& reader) noexcept {
    return skip_optional(reader, 6) && skip_optional(reader, 128U * 8U) && skip_optional(reader, 64)
           && skip_optional(reader, 64) && skip_optional(reader, 64) && skip_optional(reader, 64)
           && skip_optional(reader, 32) && skip_optional(reader, 32)
           && skip_optional(reader, 8U * 8U) && skip_optional(reader, 32)
           && skip_optional(reader, 86U * 8U) && skip_optional(reader, 86U * 8U)
           && skip_optional(reader, 86U * 8U) && skip_optional(reader, 64U * 8U)
           && skip_large_array(reader) && reader.skip(1);
}

/** The second leg is the one whose first scalars name the player's current region. */
constexpr std::size_t kRegionLegIndex = 1;

/** Reads the D4 branch and keeps its optional transition token and the player's region. */
bool read_transition_branch(encoding::bits::Reader& reader,
                            ClientAuthoritativeData& update) noexcept {
    for (std::size_t leg = 0; leg < 2; ++leg) {
        bool present = false;
        if (!read_presence(reader, present)) {
            return false;
        }
        if (!present) {
            continue;
        }
        RegionState region{};
        const bool keepRegion = leg == kRegionLegIndex;
        if (!read_d6(reader, region, keepRegion)) {
            return false;
        }
        if (keepRegion) {
            update.region = region;
            update.hasRegion = true;
        }
    }

    bool tokenPresent = false;
    std::uint64_t token = 0;
    if (!read_presence(reader, tokenPresent) || (tokenPresent && !reader.read(8, token))) {
        return false;
    }
    if (tokenPresent) {
        update.transitionToken = static_cast<std::uint8_t>(token);
        update.hasTransitionToken = true;
    }
    return skip_optional(reader, 8) && skip_optional(reader, 3);
}

} // namespace sunrise::middleware::bap::activity_message::client_authoritative_data
