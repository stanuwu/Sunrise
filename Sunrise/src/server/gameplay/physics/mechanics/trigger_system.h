#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "command_deduplicator.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Fixed trigger storage bounds definitions and actor memberships per world. */
inline constexpr std::size_t kTriggerCapacity = 32;
inline constexpr std::size_t kTriggerMembershipCapacity = 256;
inline constexpr std::uint16_t kInvalidTriggerSlot = static_cast<std::uint16_t>(kTriggerCapacity);

struct TriggerHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidTriggerSlot};
};

enum class TriggerEdge : std::uint8_t {
    enter,
    stay,
    leave,
};

enum class TriggerResult : std::uint8_t {
    applied,
    duplicate,
    noChange,
    invalidCommand,
    staleWorld,
    invalidHandle,
    invalidDefinition,
    invalidOccupancy,
    invalidTick,
    capacity,
    outputCapacity,
};

struct TriggerOccupancy final {
    TriggerHandle trigger{};
    ActorHandle actor{};
};

struct TriggerEvent final {
    TriggerHandle trigger{};
    ActorHandle actor{};
    WorldEpoch worldEpoch{};
    TickId tick{};
    std::uint64_t triggerId{};
    PolicyOwnerId policyOwnerId{};
    std::uint32_t bubble{};
    TriggerEdge edge{TriggerEdge::enter};
};

struct TriggerSlotSnapshot final {
    std::uint64_t generation{};
    std::uint64_t triggerId{};
    PolicyOwnerId policyOwnerId{};
    CommandId createCommandId{};
    std::uint32_t bubble{};
    bool active{};
};

struct TriggerSystemSnapshot final {
    std::array<TriggerSlotSnapshot, kTriggerCapacity> slots{};
    std::array<TriggerOccupancy, kTriggerMembershipCapacity> memberships{};
    CommandDeduplicatorSnapshot commands{};
    WorldEpoch worldEpoch{};
    TickId lastEvaluationTick{};
    std::size_t membershipCount{};
    bool hasEvaluation{};
};

/** Converts host-provided occupancy into deterministic trigger edges. */
class TriggerSystem final {
public:
    /** Clears every trigger and binds future commands to one world epoch. */
    void reset(WorldEpoch worldEpoch) noexcept;

    /** Creates one logical trigger without assigning activity meaning. */
    [[nodiscard]] TriggerResult create(const CommandHeader& command,
                                       std::uint64_t triggerId,
                                       std::uint32_t bubble,
                                       TriggerHandle& handle) noexcept;

    /** Removes one exact trigger generation and its retained occupancy. */
    [[nodiscard]] TriggerResult remove(const CommandHeader& command, TriggerHandle handle) noexcept;

    /** Emits enter, stay, and leave edges from the complete current occupancy set. */
    [[nodiscard]] TriggerResult evaluate(TickId tick,
                                         std::span<const TriggerOccupancy> occupancy,
                                         std::span<TriggerEvent> events,
                                         std::size_t& eventCount) noexcept;

    /** Releases command ids older than the durable replay floor. */
    void expire_commands_before(TickId firstRetainedTick) noexcept;

    /** Copies fixed trigger, occupancy, and command state. */
    [[nodiscard]] TriggerSystemSnapshot snapshot() const noexcept;

    /** Restores a validated fixed snapshot. */
    [[nodiscard]] bool restore(const TriggerSystemSnapshot& snapshot) noexcept;

private:
    [[nodiscard]] static bool equal_trigger_handle(TriggerHandle left,
                                                   TriggerHandle right) noexcept;
    [[nodiscard]] static bool occupancy_less(const TriggerOccupancy& left,
                                             const TriggerOccupancy& right) noexcept;
    [[nodiscard]] static bool equal_occupancy(const TriggerOccupancy& left,
                                              const TriggerOccupancy& right) noexcept;
    [[nodiscard]] bool valid_handle(TriggerHandle handle) const noexcept;
    void erase_memberships(TriggerHandle handle) noexcept;

    std::array<TriggerSlotSnapshot, kTriggerCapacity> slots_{};
    std::array<TriggerOccupancy, kTriggerMembershipCapacity> memberships_{};
    CommandDeduplicator commands_{};
    WorldEpoch worldEpoch_{};
    TickId lastEvaluationTick_{};
    std::size_t membershipCount_{};
    bool hasEvaluation_{};
};

} // namespace sunrise::server::gameplay::physics::mechanics
