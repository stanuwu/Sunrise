#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity::bubble_authority {

/** 64 usable bubbles and one first-send fallback own grant tokens. */
inline constexpr std::size_t kAuthoritySlotCount = 65;
/** Bubble 64 is sent with the first usable bubble and never derived from a slice set. */
inline constexpr std::uint8_t kFallbackBubble = 64;
/** Slice-set indices 0 to 511 map into the 64 usable bubbles. */
inline constexpr std::int32_t kMaximumGrantSliceSetIndex = 511;
/** Dividing a slice-set index by 8 gives its bubble index. */
inline constexpr std::uint8_t kSliceSetToBubbleShift = 3;
/** The client's cleared mirror changes when the first nonzero token arrives. */
inline constexpr std::uint16_t kInitialGrantToken = 1;
/** The cleared grant slot uses a value outside the 65-entry authority table. */
inline constexpr std::uint8_t kInvalidBubble = 0xFF;

/** One changed per-bubble token picked under the State lock. */
struct Grant final {
    std::uint8_t bubble{kInvalidBubble};
    std::uint16_t token{};
};

/** Persistent grant-token mirrors owned by one activity session. */
struct AuthorityState final {
    std::array<std::uint16_t, kAuthoritySlotCount> grantTokens{};
};

} // namespace sunrise::state::activity::bubble_authority
