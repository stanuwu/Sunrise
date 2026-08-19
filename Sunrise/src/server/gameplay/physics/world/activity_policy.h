#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "command_queue.h"

namespace sunrise::server::gameplay::physics::world {

/** Fixed capabilities and budgets declared before a world opens. */
struct ActivityPolicyManifest final {
    BlueprintRef policy{};
    std::uint64_t contentBuildId{};
    std::uint64_t allowedCommandMask{};
    std::uint32_t fixedRateHz{kDefaultFixedRateHz};
    std::uint32_t actorBudget{};
    std::uint32_t triggerBudget{};
    std::uint32_t queryBudget{};
    std::uint32_t counterBudget{};
    std::uint32_t creditBudget{};
    std::uint32_t persistenceSchemaVersion{};
};

/** Immutable world inputs supplied once to policy initialization. */
struct ActivityPolicyContext final {
    WorldIdentity world{};
    std::uint64_t deterministicSeed{};
    std::uint32_t fixedRateHz{kDefaultFixedRateHz};
};

/** Fixed delta supplied to policy callbacks without wall-clock time. */
struct PolicyTickContext final {
    TickId tick{};
    std::uint32_t fixedDeltaNumerator{kFixedDeltaNumerator};
    std::uint32_t fixedDeltaDenominator{kDefaultFixedRateHz};
};

/** Persistence sink for opaque policy-owned state only. */
class IPolicyStateWriter {
public:
    virtual ~IPolicyStateWriter() = default;
    /** @return True when all bytes entered the caller-owned snapshot. */
    [[nodiscard]] virtual bool write(std::span<const std::byte> bytes) noexcept = 0;
};

/** Persistence source for opaque policy-owned state only. */
class IPolicyStateReader {
public:
    virtual ~IPolicyStateReader() = default;
    /** @return True when the requested fixed state bytes were read. */
    [[nodiscard]] virtual bool read(std::span<std::byte> bytes) noexcept = 0;
};

/** Typed policy command sink. It has no transport or wire surface. */
class HostCommands final {
public:
    /** @return Admission status for one logical actor spawn. */
    [[nodiscard]] CommandSubmitStatus spawn_actor(const PolicyCommandMeta& meta,
                                                  const SpawnActorCommand& command) noexcept;
    /** @return Admission status for one logical actor removal. */
    [[nodiscard]] CommandSubmitStatus remove_actor(const PolicyCommandMeta& meta,
                                                   const RemoveActorCommand& command) noexcept;
    /** @return Admission status for one authority transfer. */
    [[nodiscard]] CommandSubmitStatus
    set_motion_authority(const PolicyCommandMeta& meta,
                         const SetMotionAuthorityCommand& command) noexcept;
    /** @return Admission status for one kinematic target. */
    [[nodiscard]] CommandSubmitStatus
    set_kinematic_target(const PolicyCommandMeta& meta,
                         const SetKinematicTargetCommand& command) noexcept;
    /** @return Admission status for one exact actor teleport. */
    [[nodiscard]] CommandSubmitStatus teleport_actor(const PolicyCommandMeta& meta,
                                                     const TeleportActorCommand& command) noexcept;
    /** @return Admission status for one generic controller profile. */
    [[nodiscard]] CommandSubmitStatus
    configure_controller(const PolicyCommandMeta& meta,
                         const ConfigureControllerCommand& command) noexcept;
    /** @return Admission status for one navigation request. */
    [[nodiscard]] CommandSubmitStatus request_path(const PolicyCommandMeta& meta,
                                                   const RequestPathCommand& command) noexcept;
    /** @return Admission status for one generic combat profile. */
    [[nodiscard]] CommandSubmitStatus
    configure_combat(const PolicyCommandMeta& meta, const ConfigureCombatCommand& command) noexcept;
    /** @return Admission status for one damage intent. */
    [[nodiscard]] CommandSubmitStatus submit_damage(const PolicyCommandMeta& meta,
                                                    const SubmitDamageCommand& command) noexcept;
    /** @return Admission status for one trigger creation. */
    [[nodiscard]] CommandSubmitStatus create_trigger(const PolicyCommandMeta& meta,
                                                     const CreateTriggerCommand& command) noexcept;
    /** @return Admission status for one trigger removal. */
    [[nodiscard]] CommandSubmitStatus remove_trigger(const PolicyCommandMeta& meta,
                                                     const RemoveTriggerCommand& command) noexcept;
    /** @return Admission status for one checked counter addition. */
    [[nodiscard]] CommandSubmitStatus
    add_objective_counter(const PolicyCommandMeta& meta,
                          const AddObjectiveCounterCommand& command) noexcept;
    /** @return Admission status for one checked counter assignment. */
    [[nodiscard]] CommandSubmitStatus
    set_objective_counter(const PolicyCommandMeta& meta,
                          const SetObjectiveCounterCommand& command) noexcept;
    /** @return Admission status for one participant contribution. */
    [[nodiscard]] CommandSubmitStatus
    record_participant_credit(const PolicyCommandMeta& meta,
                              const RecordParticipantCreditCommand& command) noexcept;
    /** @return Admission status for one mapped presentation incident. */
    [[nodiscard]] CommandSubmitStatus
    emit_mapped_incident(const PolicyCommandMeta& meta,
                         const EmitMappedIncidentCommand& command) noexcept;
    /** @return Admission status for one fixed-tick timer. */
    [[nodiscard]] CommandSubmitStatus
    schedule_tick_timer(const PolicyCommandMeta& meta,
                        const ScheduleTickTimerCommand& command) noexcept;
    /** @return Admission status for one immutable checkpoint request. */
    [[nodiscard]] CommandSubmitStatus
    request_checkpoint(const PolicyCommandMeta& meta,
                       const RequestCheckpointCommand& command) noexcept;
    /** @return Admission status for one durable reward intent. */
    [[nodiscard]] CommandSubmitStatus
    submit_reward_intent(const PolicyCommandMeta& meta,
                         const SubmitRewardIntentCommand& command) noexcept;

private:
    friend class WorldRunner;

    HostCommands(CommandQueue& queue,
                 const ActivityPolicyManifest& manifest,
                 WorldIdentity world,
                 CommandSource source,
                 TickId minimumTick,
                 std::uint64_t& sourceSequence) noexcept;

    [[nodiscard]] CommandSubmitStatus submit(const PolicyCommandMeta& meta,
                                             HostCommandPayload payload) noexcept;

    CommandQueue* queue_{};
    const ActivityPolicyManifest* manifest_{};
    WorldIdentity world_{};
    CommandSource source_{};
    TickId minimumTick_{};
    std::uint64_t* sourceSequence_{};
};

/** Mission-policy boundary with no socket, wire, token, or wall-clock access. */
class IActivityPolicy {
public:
    virtual ~IActivityPolicy() = default;
    /** @return Fixed policy identity, capabilities, and budgets. */
    [[nodiscard]] virtual ActivityPolicyManifest manifest() const noexcept = 0;
    /** @return True when initial policy state and commands are valid. */
    [[nodiscard]] virtual bool initialize(const ActivityPolicyContext& context,
                                          HostCommands& commands) noexcept = 0;
    /** Runs before generic services for one fixed tick. */
    virtual void pre_tick(const PolicyTickContext& context, HostCommands& commands) noexcept = 0;
    /** Reads immutable committed events and may queue future commands. */
    virtual void post_tick(const PolicyTickContext& context,
                           std::span<const CommittedEvent> committedEvents,
                           HostCommands& commands) noexcept = 0;
    /** @return True when all policy-owned state was saved. */
    [[nodiscard]] virtual bool save(IPolicyStateWriter& writer) const noexcept = 0;
    /** @return True when compatible policy-owned state was loaded. */
    [[nodiscard]] virtual bool load(IPolicyStateReader& reader) noexcept = 0;
};

/** Empty deterministic policy used when no mission engine is installed. */
class DefaultActivityPolicy final : public IActivityPolicy {
public:
    /** @return Zero command permissions and zero dynamic actor budget. */
    [[nodiscard]] ActivityPolicyManifest manifest() const noexcept override;
    /** @return True without creating dynamic actors. */
    [[nodiscard]] bool initialize(const ActivityPolicyContext& context,
                                  HostCommands& commands) noexcept override;
    /** Performs one deterministic no-op pre-tick callback. */
    void pre_tick(const PolicyTickContext& context, HostCommands& commands) noexcept override;
    /** Performs one deterministic no-op post-tick callback. */
    void post_tick(const PolicyTickContext& context,
                   std::span<const CommittedEvent> committedEvents,
                   HostCommands& commands) noexcept override;
    /** @return True because fallback policy state is empty. */
    [[nodiscard]] bool save(IPolicyStateWriter& writer) const noexcept override;
    /** @return True because fallback policy state is empty. */
    [[nodiscard]] bool load(IPolicyStateReader& reader) noexcept override;
};

} // namespace sunrise::server::gameplay::physics::world
