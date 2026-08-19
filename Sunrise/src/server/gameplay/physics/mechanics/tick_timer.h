#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "command_deduplicator.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** One world retains at most 64 deterministic tick timers. */
inline constexpr std::size_t kTickTimerCapacity = 64;
/** The invalid timer slot lies immediately outside fixed timer storage. */
inline constexpr std::uint16_t kInvalidTickTimerSlot =
    static_cast<std::uint16_t>(kTickTimerCapacity);

struct TickTimerHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidTickTimerSlot};
};

enum class TickTimerResult : std::uint8_t {
    applied,
    duplicate,
    noChange,
    invalidCommand,
    staleWorld,
    invalidHandle,
    invalidTick,
    capacity,
    outputCapacity,
};

struct TickTimerEvent final {
    TickTimerHandle timer{};
    WorldEpoch worldEpoch{};
    TickId scheduledTick{};
    TickId firedTick{};
    std::uint64_t timerId{};
    PolicyOwnerId policyOwnerId{};
    CommandId scheduleCommandId{};
};

struct TickTimerSlotSnapshot final {
    std::uint64_t generation{};
    std::uint64_t timerId{};
    TickId fireTick{};
    PolicyOwnerId policyOwnerId{};
    CommandId scheduleCommandId{};
    bool active{};
};

struct TickTimerServiceSnapshot final {
    std::array<TickTimerSlotSnapshot, kTickTimerCapacity> slots{};
    CommandDeduplicatorSnapshot commands{};
    WorldEpoch worldEpoch{};
};

/** Fixed one-shot timers driven only by integer world ticks. */
class TickTimerService final {
public:
    /** Clears all timers and binds future commands to one world epoch. */
    void reset(WorldEpoch worldEpoch) noexcept;

    /** Schedules one idempotent timer. */
    [[nodiscard]] TickTimerResult schedule(const CommandHeader& command,
                                           std::uint64_t timerId,
                                           TickId fireTick,
                                           TickTimerHandle& handle) noexcept;

    /** Cancels one exact timer generation. */
    [[nodiscard]] TickTimerResult cancel(const CommandHeader& command,
                                         TickTimerHandle handle) noexcept;

    /** Fires every due timer in stable scheduled order. */
    [[nodiscard]] TickTimerResult
    fire_due(TickId tick, std::span<TickTimerEvent> events, std::size_t& eventCount) noexcept;

    /** Releases command ids older than the durable replay floor. */
    void expire_commands_before(TickId firstRetainedTick) noexcept;

    /** Copies fixed timer and command state. */
    [[nodiscard]] TickTimerServiceSnapshot snapshot() const noexcept;

    /** Restores a validated fixed snapshot. */
    [[nodiscard]] bool restore(const TickTimerServiceSnapshot& snapshot) noexcept;

private:
    [[nodiscard]] bool valid_handle(TickTimerHandle handle) const noexcept;

    std::array<TickTimerSlotSnapshot, kTickTimerCapacity> slots_{};
    CommandDeduplicator commands_{};
    WorldEpoch worldEpoch_{};
};

} // namespace sunrise::server::gameplay::physics::mechanics
