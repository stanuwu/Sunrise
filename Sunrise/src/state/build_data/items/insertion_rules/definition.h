#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../rewards/definition.h"

namespace sunrise::state::build_data::items::insertion_rules {

/**
 * Rule rows. The installed build declares insertion rules on 1,809 plugs. A build declaring more
 * settles the domain unsupported.
 */
inline constexpr std::size_t kRuleCapacity = 2048;

/**
 * Instructions one plug's rules expand to, joined by AND. A longer list is kept as an unreadable
 * row, which refuses the plug wherever its rules are checked.
 */
inline constexpr std::size_t kConditionCapacity = 16;

/** Rules that require no single account flag carry this instead of a flag row. */
inline constexpr std::uint16_t kUnavailableFlagIndex = 0xFFFFU;

/** The insertion rules one plug declares, bound to the saved banks. */
struct Rule {
    std::uint16_t plugDefinitionIndex{};
    /** Account flag row every rule together requires set, or kUnavailableFlagIndex. */
    std::uint16_t requiredFlagIndex{kUnavailableFlagIndex};
    /** Every rule must hold; empty when the rules did not read, bind or fit. */
    std::array<rewards::Instruction, kConditionCapacity> condition{};
    std::uint8_t conditionCount{};
    /** False when the rules did not read, bind or fit, which refuses the plug. */
    bool readable{};
};

} // namespace sunrise::state::build_data::items::insertion_rules
