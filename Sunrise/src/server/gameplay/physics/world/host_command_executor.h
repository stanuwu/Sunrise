#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "host_command.h"

namespace sunrise::server::gameplay::physics::world {

enum class HostExecutionPhase : std::uint8_t {
    commandCommit,
    serviceTick,
};

/** Exact writer phase with fixed delta and no wall-clock input. */
struct HostExecutionContext final {
    WorldIdentity world{};
    TickId tick{};
    std::size_t commandOrdinal{};
    std::uint32_t fixedDeltaNumerator{kFixedDeltaNumerator};
    std::uint32_t fixedDeltaDenominator{kDefaultFixedRateHz};
    std::uint32_t triggerBudget{};
    std::uint32_t queryBudget{};
    std::uint32_t counterBudget{};
    std::uint32_t creditBudget{};
    HostExecutionPhase phase{HostExecutionPhase::commandCommit};
};

enum class HostCommandExecutionStatus : std::uint8_t {
    applied,
    rejected,
    unhealthy,
};

/** Bounded synchronous command result returned on the world writer. */
struct HostCommandExecutionResult final {
    std::size_t eventCount{};
    CommandRejectReason rejectReason{CommandRejectReason::invalid};
    HostCommandExecutionStatus status{HostCommandExecutionStatus::rejected};
};

/** Bounded generic-service result returned before policy post-tick. */
struct HostTickExecutionResult final {
    std::size_t eventCount{};
    bool healthy{true};
    std::span<const CanonicalPoseCommit> poseCommits{};
};

/** Generic service seam with no transport, wire, token, or mutable actor access. */
class IHostCommandExecutor {
public:
    virtual ~IHostCommandExecutor() = default;

    /**
     * Executes one non-core command synchronously on the world writer.
     * @param context Exact command phase and fixed tick.
     * @param command Ordered typed command.
     * @param actors Read-only actor state at this command boundary.
     * @param events Fixed caller sink for copied committed events.
     * @return Applied, rejected, or unhealthy with a bounded event count.
     */
    [[nodiscard]] virtual HostCommandExecutionResult
    execute_command(const HostExecutionContext& context,
                    const HostCommand& command,
                    std::span<const ActorSnapshot> actors,
                    std::span<CommittedEvent> events) noexcept = 0;

    /**
     * Steps generic services after policy pre-tick and before policy post-tick.
     * @param context Exact service phase and fixed tick.
     * @param actors Read-only actor state after command commit.
     * @param committedEvents Read-only events already committed this tick.
     * @param events Fixed caller sink for copied service events.
     * @return Health, bounded events, and a borrowed atomic pose batch.
     */
    [[nodiscard]] virtual HostTickExecutionResult
    step_services(const HostExecutionContext& context,
                  std::span<const ActorSnapshot> actors,
                  std::span<const CommittedEvent> committedEvents,
                  std::span<CommittedEvent> events) noexcept = 0;
};

} // namespace sunrise::server::gameplay::physics::world
