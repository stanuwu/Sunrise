#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "command_deduplicator.h"

namespace sunrise::server::gameplay::physics::mechanics {

/** Fixed objective storage bounds counters and threshold edges per world. */
inline constexpr std::size_t kObjectiveCounterCapacity = 32;
inline constexpr std::size_t kObjectiveThresholdCapacity = 8;
/** The invalid objective slot lies immediately outside fixed counter storage. */
inline constexpr std::uint16_t kInvalidObjectiveCounterSlot =
    static_cast<std::uint16_t>(kObjectiveCounterCapacity);

struct ObjectiveCounterHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidObjectiveCounterSlot};
};

enum class ObjectivePolicy : std::uint8_t {
    monotonic,
    bounded,
};

enum class ObjectiveOperation : std::uint8_t {
    add,
    set,
};

enum class ObjectiveEventKind : std::uint8_t {
    valueChanged,
    thresholdCrossedUp,
    thresholdCrossedDown,
};

enum class ObjectiveResult : std::uint8_t {
    applied,
    duplicate,
    noChange,
    invalidCommand,
    staleWorld,
    invalidHandle,
    invalidDefinition,
    arithmeticOverflow,
    boundViolation,
    monotonicViolation,
    revisionExhausted,
    capacity,
    outputCapacity,
};

struct ObjectiveCounterDefinition final {
    std::array<std::int64_t, kObjectiveThresholdCapacity> thresholds{};
    std::uint64_t counterId{};
    PolicyOwnerId policyOwnerId{};
    std::int64_t initialValue{};
    std::int64_t minimumValue{};
    std::int64_t maximumValue{};
    std::size_t thresholdCount{};
    ObjectivePolicy policy{ObjectivePolicy::monotonic};
};

struct ObjectiveMutation final {
    CommandHeader command{};
    ObjectiveCounterHandle counter{};
    std::int64_t value{};
    ObjectiveOperation operation{ObjectiveOperation::add};
};

struct ObjectiveEvent final {
    ObjectiveCounterHandle counter{};
    WorldEpoch worldEpoch{};
    TickId tick{};
    std::uint64_t counterId{};
    PolicyOwnerId policyOwnerId{};
    std::uint64_t revision{};
    std::int64_t oldValue{};
    std::int64_t newValue{};
    std::int64_t threshold{};
    ObjectiveEventKind kind{ObjectiveEventKind::valueChanged};
};

struct ObjectiveCounterSnapshot final {
    ObjectiveCounterDefinition definition{};
    std::uint64_t generation{};
    std::uint64_t revision{};
    std::int64_t value{};
    bool active{};
};

struct ObjectiveLedgerSnapshot final {
    std::array<ObjectiveCounterSnapshot, kObjectiveCounterCapacity> counters{};
    CommandDeduplicatorSnapshot commands{};
    WorldEpoch worldEpoch{};
};

/** Fixed idempotent objective counters without activity-specific meaning. */
class ObjectiveLedger final {
public:
    /** Clears every counter and binds future mutations to one world epoch. */
    void reset(WorldEpoch worldEpoch) noexcept;

    /** Creates one monotonic or bounded counter. */
    [[nodiscard]] ObjectiveResult create_counter(const ObjectiveCounterDefinition& definition,
                                                 ObjectiveCounterHandle& handle) noexcept;

    /** Removes one exact counter generation. */
    [[nodiscard]] ObjectiveResult remove_counter(ObjectiveCounterHandle handle) noexcept;

    /** Applies one checked mutation and emits copied value and threshold events. */
    [[nodiscard]] ObjectiveResult apply(const ObjectiveMutation& mutation,
                                        std::span<ObjectiveEvent> events,
                                        std::size_t& eventCount) noexcept;

    /** Releases command ids older than the durable replay floor. */
    void expire_commands_before(TickId firstRetainedTick) noexcept;

    /** Copies one current counter without exposing mutable storage. */
    [[nodiscard]] bool query(ObjectiveCounterHandle handle,
                             ObjectiveCounterSnapshot& counter) const noexcept;

    /** Copies fixed counter and command state. */
    [[nodiscard]] ObjectiveLedgerSnapshot snapshot() const noexcept;

    /** Restores a validated fixed snapshot. */
    [[nodiscard]] bool restore(const ObjectiveLedgerSnapshot& snapshot) noexcept;

private:
    [[nodiscard]] static bool
    valid_definition(const ObjectiveCounterDefinition& definition) noexcept;
    [[nodiscard]] bool valid_handle(ObjectiveCounterHandle handle) const noexcept;

    std::array<ObjectiveCounterSnapshot, kObjectiveCounterCapacity> counters_{};
    CommandDeduplicator commands_{};
    WorldEpoch worldEpoch_{};
};

} // namespace sunrise::server::gameplay::physics::mechanics
