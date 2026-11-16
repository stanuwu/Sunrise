#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../../state/activity_sdk/format.h"

namespace sunrise::server::activity::mission {

/** Watched objective sensors retained per instance. */
inline constexpr std::size_t kObjectiveObservationCapacity = 32;
/** One objective sensor carries this many objective blocks. */
inline constexpr std::size_t kObjectiveCapacity = 24;
/** One objective block carries this many task counters, one per authored task of its group. */
inline constexpr std::size_t kObjectiveTaskCapacity = 24;
static_assert(kObjectiveTaskCapacity == state::activity_sdk::format::kCombatTaskCapacity);
static_assert(kObjectiveTaskCapacity <= std::numeric_limits<std::uint32_t>::digits);

/** One objective sensor's retained task counters, with the slot they were reported for. */
struct ObjectiveLevelRow final {
    /** One bit per counter the client reported; an unreported counter is absent, not zero. */
    std::array<std::uint32_t, kObjectiveCapacity> known{};
    std::array<std::array<std::uint8_t, kObjectiveTaskCapacity>, kObjectiveCapacity> counters{};
    /** Link whose reports these levels were read from. */
    std::uint64_t sourceGeneration{};
    std::uint32_t registryKey{};
    std::uint32_t objectTag{};
    std::uint16_t slotIndex{};
    bool used{};
};

} // namespace sunrise::server::activity::mission
