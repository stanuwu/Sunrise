#pragma once

#include <cstdint>
#include <limits>

namespace sunrise::server::gameplay::physics::mechanics {

using WorldEpoch = std::uint64_t;
using TickId = std::uint64_t;
using CommandId = std::uint64_t;
using PolicyOwnerId = std::uint64_t;

/** Zero is reserved as the absent value for every mechanics identity domain. */
inline constexpr WorldEpoch kAbsentWorldEpoch = 0;
inline constexpr CommandId kAbsentCommandId = 0;
inline constexpr PolicyOwnerId kAbsentPolicyOwnerId = 0;
inline constexpr std::uint64_t kAbsentActorId = 0;
inline constexpr std::uint64_t kAbsentGeneration = 0;

/** Common identity carried by every future activity-policy command. */
struct CommandHeader final {
    WorldEpoch worldEpoch{};
    CommandId commandId{};
    TickId executionTick{};
    PolicyOwnerId policyOwnerId{};
    std::uint32_t blueprintVersion{};
};

/** Stable actor identity plus the generation which owns it now. */
struct ActorHandle final {
    std::uint64_t actorId{};
    std::uint64_t generation{};
};

/** Integer world position used by deterministic mechanics checks. */
struct PositionMillimeters final {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
};

/** @return True when the command can belong to the expected world. */
[[nodiscard]] constexpr bool valid_command_header(const CommandHeader& header,
                                                  WorldEpoch expectedWorldEpoch) noexcept {
    return expectedWorldEpoch != kAbsentWorldEpoch && header.worldEpoch == expectedWorldEpoch
           && header.commandId != kAbsentCommandId && header.policyOwnerId != kAbsentPolicyOwnerId
           && header.executionTick != 0 && header.blueprintVersion != 0;
}

/** @return True when the handle identifies one actor generation. */
[[nodiscard]] constexpr bool valid_actor_handle(const ActorHandle& actor) noexcept {
    return actor.actorId != kAbsentActorId && actor.generation != kAbsentGeneration;
}

/** @return True when two handles identify the same actor generation. */
[[nodiscard]] constexpr bool equal_actor_handle(const ActorHandle& left,
                                                const ActorHandle& right) noexcept {
    return left.actorId == right.actorId && left.generation == right.generation;
}

/** @return The next process-local generation, or zero when the source is exhausted. */
[[nodiscard]] constexpr std::uint64_t next_generation(std::uint64_t generation) noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        return kAbsentGeneration;
    }
    return generation + 1;
}

} // namespace sunrise::server::gameplay::physics::mechanics
