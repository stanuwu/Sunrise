#pragma once

#include <cstdint>

#include "common_state.h"

namespace sunrise::middleware::gameplay::external {

/** Result of reading the receive-only empty-channel profile. */
enum class EmptyProfileResult : std::uint8_t {
    accepted,
    malformed,
    channel0Present,
    channel1Present,
    channel2Present,
    channel3Present,
};

/** Common state and bounded fields retained by the empty profile. */
struct EmptyProfile {
    CommonState common{};
    std::uint8_t defaultBubble{};
    bool commonPresent{};
    bool defaultBubblePresent{};
    bool channel3TrailingList{};
};

/** Reads common state and four empty channels without changing output on failure. */
[[nodiscard]] EmptyProfileResult read_empty_profile(encoding::bits::Reader& reader,
                                                    EmptyProfile& output) noexcept;

} // namespace sunrise::middleware::gameplay::external
