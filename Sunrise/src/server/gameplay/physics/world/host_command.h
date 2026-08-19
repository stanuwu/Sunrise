#pragma once

#include <cstdint>
#include <variant>

#include "world_types.h"

namespace sunrise::server::gameplay::physics::world {

/** Header supplied by an activity policy. The host stamps world and source identity. */
struct PolicyCommandMeta final {
    BlueprintRef definition{};
    CommandId commandId{};
    TickId executeTick{};
    PolicyOwnerId policyOwnerId{};
};

/** Creates one policy-owned logical actor without allocating wire identity. */
struct SpawnActorCommand final {
    ActorKey actor{};
    ActorKey parent{};
    Transform transform{};
    std::uint32_t bubble{};
    std::uint64_t initialAuthorityGeneration{kFirstGeneration};
    std::uint64_t authorityOwnerMemberId{};
    MotionAuthorityMode authorityMode{MotionAuthorityMode::host};
};

/** Removes one exact logical actor generation. */
struct RemoveActorCommand final {
    ActorKey actor{};
};

/** Transfers motion ownership through one strict next epoch. */
struct SetMotionAuthorityCommand final {
    ActorKey actor{};
    std::uint64_t expectedAuthorityGeneration{};
    std::uint64_t nextAuthorityGeneration{};
    std::uint64_t ownerMemberId{};
    TickId validFromTick{};
    MotionAuthorityMode mode{MotionAuthorityMode::host};
};

/** Replaces one host-checked motion target. */
struct SetKinematicTargetCommand final {
    ActorKey actor{};
    Transform target{};
    std::uint64_t authorityGeneration{};
};

/** Moves one exact actor immediately within a fixed tick. */
struct TeleportActorCommand final {
    ActorKey actor{};
    Transform target{};
    std::uint64_t authorityGeneration{};
};

/** Selects versioned generic controller mechanics for one actor. */
struct ConfigureControllerCommand final {
    ActorKey actor{};
    ActorKey targetActor{};
    Vector3 targetPosition{};
    BlueprintRef controller{};
};

/** Requests a bounded navigation path for one actor. */
struct RequestPathCommand final {
    ActorKey actor{};
    Vector3 destination{};
    BlueprintRef navigationProfile{};
};

/** Selects one versioned generic combat profile. */
struct ConfigureCombatCommand final {
    ActorKey actor{};
    BlueprintRef combatProfile{};
};

/** Proposes one authority-checked generic damage mutation. */
struct SubmitDamageCommand final {
    ActorKey sourceActor{};
    ActorKey targetActor{};
    BlueprintRef attack{};
    TickId claimedTick{};
    std::uint64_t amount{};
};

/** Creates one policy-owned trigger volume. */
struct CreateTriggerCommand final {
    std::uint64_t triggerId{};
    Transform transform{};
    Vector3 halfExtents{};
    BlueprintRef triggerProfile{};
};

/** Removes one exact trigger generation. */
struct RemoveTriggerCommand final {
    std::uint64_t triggerId{};
    std::uint64_t triggerGeneration{};
};

/** Adds a checked value to one generic objective counter. */
struct AddObjectiveCounterCommand final {
    std::uint64_t counterId{};
    std::int64_t amount{};
    std::uint64_t expectedRevision{};
};

/** Sets one generic objective counter at an exact revision. */
struct SetObjectiveCounterCommand final {
    std::uint64_t counterId{};
    std::int64_t value{};
    std::uint64_t expectedRevision{};
};

/** Records one deterministic participant contribution. */
struct RecordParticipantCreditCommand final {
    std::uint64_t participantId{};
    std::uint64_t channelId{};
    std::uint64_t amount{};
};

/** Requests one allowlisted presentation incident by logical actors. */
struct EmitMappedIncidentCommand final {
    std::uint64_t incidentId{};
    ActorKey sourceActor{};
    ActorKey targetActor{};
};

/** Schedules one deterministic timer by fixed tick. */
struct ScheduleTickTimerCommand final {
    std::uint64_t timerId{};
    TickId fireTick{};
    std::uint64_t timerGeneration{};
};

/** Requests an immutable logical-world checkpoint. */
struct RequestCheckpointCommand final {
    std::uint64_t reasonId{};
};

/** Proposes one durable idempotent reward intent. */
struct SubmitRewardIntentCommand final {
    std::uint64_t participantId{};
    std::uint64_t rewardIntentId{};
    BlueprintRef rewardTable{};
};

/** Closed typed command set available to the generic host. */
using HostCommandPayload = std::variant<SpawnActorCommand,
                                        RemoveActorCommand,
                                        SetMotionAuthorityCommand,
                                        SetKinematicTargetCommand,
                                        TeleportActorCommand,
                                        ConfigureControllerCommand,
                                        RequestPathCommand,
                                        ConfigureCombatCommand,
                                        SubmitDamageCommand,
                                        CreateTriggerCommand,
                                        RemoveTriggerCommand,
                                        AddObjectiveCounterCommand,
                                        SetObjectiveCounterCommand,
                                        RecordParticipantCreditCommand,
                                        EmitMappedIncidentCommand,
                                        ScheduleTickTimerCommand,
                                        RequestCheckpointCommand,
                                        SubmitRewardIntentCommand>;

/** Fully stamped command stored by the bounded ingress queue. */
struct HostCommand final {
    HostCommandHeader header{};
    HostCommandPayload payload{RequestCheckpointCommand{}};
};

/** Stable variant index used by policy manifests. */
enum class HostCommandKind : std::uint8_t {
    spawnActor,
    removeActor,
    setMotionAuthority,
    setKinematicTarget,
    teleportActor,
    configureController,
    requestPath,
    configureCombat,
    submitDamage,
    createTrigger,
    removeTrigger,
    addObjectiveCounter,
    setObjectiveCounter,
    recordParticipantCredit,
    emitMappedIncident,
    scheduleTickTimer,
    requestCheckpoint,
    submitRewardIntent,
    count,
};

/** @return Stable kind matching the closed payload variant index. */
[[nodiscard]] HostCommandKind command_kind(const HostCommand& command) noexcept;
/** @return True when a command reads or mutates logical actor state. */
[[nodiscard]] bool command_uses_actor(HostCommandKind kind) noexcept;
/** @return One manifest bit, or zero for an invalid kind. */
[[nodiscard]] std::uint64_t command_kind_mask(HostCommandKind kind) noexcept;
/** @return True when all required identity and order fields are nonzero. */
[[nodiscard]] bool valid_command_header(const HostCommandHeader& header) noexcept;

} // namespace sunrise::server::gameplay::physics::world
