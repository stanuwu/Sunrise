#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state {

/** The native family-5 lists hold 100 rows each. The 7-bit wire count is not the limit. */
inline constexpr std::size_t kUnlockOverrideCapacity = 100;

/**
 * A flag override of this value reads as false; zero is no override. The client keeps a flag it
 * evaluated after the override that set it is removed, so a flag is cleared with this value.
 */
inline constexpr std::uint8_t kFlagOverrideFalse = 1;

/** One logical unlock-flag value stored by slot. */
struct UnlockFlagOverride {
    std::uint16_t slot{};
    std::uint8_t value{};
};

/** One logical signed unlock value stored by slot. */
struct UnlockValueOverride {
    std::uint16_t slot{};
    std::int32_t value{};
};

/** Global family-5 object and its bounded account overrides. */
struct Family5State {
    std::uint64_t objectSoid{};
    std::array<UnlockFlagOverride, kUnlockOverrideCapacity> flags{};
    std::size_t flagCount{};
    std::array<UnlockValueOverride, kUnlockOverrideCapacity> values{};
    std::size_t valueCount{};
    bool contentGateArm{};
};

/** Account-wide evaluated content state. */
struct InvestmentState {
    Family5State family5;
};

} // namespace sunrise::state
