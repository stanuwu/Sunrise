#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "activity_policy.h"
#include "authority_manager.h"
#include "host_command_executor.h"

namespace sunrise::server::gameplay::physics::world {

/** Inputs fixed for one world lifetime. */
struct WorldOpenConfig final {
    IHostCommandExecutor* executor{};
    std::uint64_t activitySessionId{};
    std::uint64_t ownerGeneration{};
    std::uint64_t deterministicSeed{};
    bool allowHostActorCommands{};
};

/** Bounded scheduler result with any remaining tick debt. */
struct AdvanceResult final {
    TickId lastTick{};
    std::uint32_t ticksRun{};
    std::uint32_t ticksDeferred{};
    bool healthy{};
};

/** Single-writer fixed-tick owner for logical world state. */
class WorldRunner final {
public:
    WorldRunner() = default;
    WorldRunner(const WorldRunner&) = delete;
    WorldRunner& operator=(const WorldRunner&) = delete;

    /** @return True when one fresh world and its selected policy initialize. */
    [[nodiscard]] bool open(const WorldOpenConfig& config,
                            IActivityPolicy* policy = nullptr) noexcept;
    /** Clears the world but preserves its process-local generation source. */
    void close() noexcept;
    /** @return Admission status for one fully stamped future command. */
    [[nodiscard]] CommandSubmitStatus submit(const HostCommand& command) noexcept;
    /** @return Bounded fixed-tick work and deferred debt. */
    [[nodiscard]] AdvanceResult advance(std::uint32_t dueTicks) noexcept;
    /** @return True when a pointer-free world snapshot was copied. */
    [[nodiscard]] bool snapshot(WorldSnapshot& output) const noexcept;
    /** @return True when a fresh runner atomically accepted logical state. */
    [[nodiscard]] bool restore(const WorldSnapshot& snapshot) noexcept;
    /**
     * Releases commands below a caller-confirmed durable replay floor.
     * The durable ingress must reject those released identities before this call.
     * @return True when the bounded floor was valid.
     */
    [[nodiscard]] bool expire_commands_before(TickId firstRetainedTick) noexcept;

    /** @return True while one world owns the runner. */
    [[nodiscard]] bool is_open() const noexcept;
    /** @return False after critical overflow or excessive tick debt. */
    [[nodiscard]] bool healthy() const noexcept;
    /** @return Exact current world, owner, and activity generations. */
    [[nodiscard]] WorldIdentity identity() const noexcept;
    /** @return Last completed fixed tick. */
    [[nodiscard]] TickId tick() const noexcept;
    /** @return Fixed rate selected when the world opened. */
    [[nodiscard]] std::uint32_t fixed_rate_hz() const noexcept;
    /** @return Number of live logical actors. */
    [[nodiscard]] std::size_t actor_count() const noexcept;
    /** @return Number of admitted future commands. */
    [[nodiscard]] std::size_t queued_command_count() const noexcept;
    /** @return Read-only events from the last committed tick. */
    [[nodiscard]] std::span<const CommittedEvent> committed_events() const noexcept;
    /** @return Exact applied commands exposed by the last service boundary. */
    [[nodiscard]] std::span<const HostCommand> committed_commands() const noexcept;
    /** @return Bounded counters owned by this world. */
    [[nodiscard]] const WorldMetrics& metrics() const noexcept;

private:
    [[nodiscard]] bool run_tick() noexcept;
    [[nodiscard]] bool begin_tick_transaction() noexcept;
    void commit_tick_transaction() noexcept;
    void rollback_tick_transaction() noexcept;
    void apply_command(const HostCommand& command, std::size_t commandOrdinal) noexcept;
    [[nodiscard]] bool execute_extension_command(const HostCommand& command,
                                                 std::size_t commandOrdinal) noexcept;
    [[nodiscard]] bool policy_actor_access(const HostCommand& command) const noexcept;
    void finalize_committed_commands(std::size_t commandCount, bool includeExtensions) noexcept;
    [[nodiscard]] bool step_extension_services(std::size_t commandCount) noexcept;
    [[nodiscard]] bool commit_service_poses(std::span<const CanonicalPoseCommit> commits,
                                            std::span<CommittedEvent> events) noexcept;
    void publish_event(const HostCommand& command,
                       CommittedEventKind kind,
                       const ActorSnapshot& actor,
                       std::uint64_t value = 0) noexcept;
    void reject(CommandRejectReason reason) noexcept;
    [[nodiscard]] bool seen_command(const HostCommandHeader& header) const noexcept;
    void remember_command(const HostCommandHeader& header) noexcept;
    [[nodiscard]] std::uint64_t compute_hash() const noexcept;

    DefaultActivityPolicy defaultPolicy_{};
    IActivityPolicy* policy_{};
    ActivityPolicyManifest manifest_{};
    ActivityPolicyContext policyContext_{};
    CommandQueue commandQueue_{};
    ActorStore actors_{};
    AuthorityManager authority_{};
    IHostCommandExecutor* executor_{};
    std::array<HostCommand, kCommandQueueCapacity> tickCommands_{};
    std::array<HostCommand, kCommandQueueCapacity> committedCommands_{};
    std::array<bool, kCommandQueueCapacity> tickCommandApplied_{};
    std::array<bool, kCommandQueueCapacity> tickCommandExtension_{};
    std::array<CommittedEvent, kCommittedEventCapacity> committedEvents_{};
    std::array<ActorSnapshot, kActorCapacity> executorActors_{};
    std::array<HostCommandHeader, kCommandHistoryCapacity> commandHistory_{};
    ActorStore rollbackActors_{};
    std::array<HostCommandHeader, kCommandHistoryCapacity> rollbackCommandHistory_{};
    std::array<CommittedEvent, kCommittedEventCapacity> rollbackCommittedEvents_{};
    std::array<HostCommand, kCommandQueueCapacity> rollbackCommittedCommands_{};
    std::array<std::byte, kPolicyRollbackCapacity> rollbackPolicyState_{};
    WorldMetrics rollbackMetrics_{};
    WorldMetrics metrics_{};
    WorldIdentity identity_{};
    TickId tick_{};
    std::uint64_t nextWorldGeneration_{kFirstGeneration};
    std::uint64_t policySourceSequence_{};
    std::uint64_t canonicalHash_{};
    TickId rollbackTick_{};
    std::uint64_t rollbackPolicySourceSequence_{};
    std::uint64_t rollbackCanonicalHash_{};
    std::size_t committedEventCount_{};
    std::size_t committedCommandCount_{};
    std::size_t commandHistoryCount_{};
    std::size_t commandHistoryCursor_{};
    std::size_t rollbackCommittedEventCount_{};
    std::size_t rollbackCommittedCommandCount_{};
    std::size_t rollbackCommandHistoryCount_{};
    std::size_t rollbackCommandHistoryCursor_{};
    std::size_t rollbackPolicyStateSize_{};
    std::uint32_t tickDebt_{};
    bool open_{};
    bool healthy_{};
    bool allowHostActorCommands_{};
    bool queueOverflowObserved_{};
    bool tickTransactionActive_{};
};

} // namespace sunrise::server::gameplay::physics::world
