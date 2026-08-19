#include "external_empty_profile.h"

namespace sunrise::middleware::gameplay::external {
namespace {

/** Presence and empty-list fields are one bit. */
constexpr std::uint8_t kFlagWidth = 1;
/** A channel-2 default bubble is one byte. */
constexpr std::uint8_t kBubbleWidth = 8;

/** Reads one boolean field. */
[[nodiscard]] bool read_flag(encoding::bits::Reader& reader, bool& value) noexcept {
    std::uint64_t field = 0;
    if (!reader.read(kFlagWidth, field)) {
        return false;
    }
    value = field != 0;
    return true;
}

} // namespace

/** Reads the receive-only common root and empty-channel profile. */
EmptyProfileResult read_empty_profile(encoding::bits::Reader& reader,
                                      EmptyProfile& output) noexcept {
    EmptyProfile candidate{};
    if (!read_flag(reader, candidate.commonPresent)) {
        return EmptyProfileResult::malformed;
    }
    if (candidate.commonPresent && !read_common_state(reader, candidate.common)) {
        return EmptyProfileResult::malformed;
    }

    bool present = false;
    if (!read_flag(reader, present)) {
        return EmptyProfileResult::malformed;
    }
    if (present) {
        return EmptyProfileResult::channel0Present;
    }
    if (!read_flag(reader, present)) {
        return EmptyProfileResult::malformed;
    }
    if (present) {
        return EmptyProfileResult::channel1Present;
    }
    if (!read_flag(reader, present)) {
        return EmptyProfileResult::malformed;
    }
    if (present) {
        return EmptyProfileResult::channel2Present;
    }
    if (!read_flag(reader, candidate.defaultBubblePresent)) {
        return EmptyProfileResult::malformed;
    }
    if (candidate.defaultBubblePresent) {
        std::uint64_t bubble = 0;
        if (!reader.read(kBubbleWidth, bubble)) {
            return EmptyProfileResult::malformed;
        }
        candidate.defaultBubble = static_cast<std::uint8_t>(bubble);
    }
    if (!read_flag(reader, present)) {
        return EmptyProfileResult::malformed;
    }
    if (present) {
        return EmptyProfileResult::channel3Present;
    }
    if (!read_flag(reader, candidate.channel3TrailingList)) {
        return EmptyProfileResult::malformed;
    }

    output = candidate;
    return EmptyProfileResult::accepted;
}

} // namespace sunrise::middleware::gameplay::external
