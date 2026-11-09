#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../items/details/definition.h"

namespace sunrise::state::build_data::pursuits {

/** A native item reference uses all bits set when it is absent. */
inline constexpr std::uint16_t kAbsent = items::details::kUnavailableItemIndex;
/** The shipped build declares 1,941 pursuit contracts. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 4096;
/** The installed pursuit objective block supports seven displayed objectives. */
inline constexpr std::size_t kObjectiveCapacity = 7;

/** The installed contract of one pursuit item: its objectives and how they combine. */
struct Definition {
    std::uint16_t definitionIndex{kAbsent};
    std::uint32_t definitionHash{};
    bool readable{};
    /** Positive for a bounty, which expires this many seconds after it is acquired. */
    std::int32_t lifetimeSeconds{};
    bool requireAllObjectives{};
    bool requireNonOptionalCompletion{};
    /** Objective-table indices. Slots past objectiveCount stay zero. */
    std::array<std::uint16_t, kObjectiveCapacity> objectives{};
    std::uint8_t objectiveCount{};
};

} // namespace sunrise::state::build_data::pursuits
