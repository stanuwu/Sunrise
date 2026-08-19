#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "server/gameplay/physics/authority/motion_validator.h"
#include "server/gameplay/physics/backend/navigation_backend.h"
#include "server/gameplay/physics/common/common_sync.h"
#include "server/gameplay/physics/controller/actor_controller_service.h"
#include "server/gameplay/physics/interest/interest_manager.h"
#include "server/gameplay/physics/mechanics/host_mechanics.h"
#include "server/gameplay/physics/persistence/checkpoint.h"
#include "server/gameplay/physics/persistence/command_journal.h"
#include "server/gameplay/physics/replication/runtime.h"
#include "server/gameplay/physics/world/world_runner.h"

namespace sunrise::server::gameplay::physics::host {

/** Fixed host limits bound world, peer, checkpoint, incident, and fallback work. */
inline constexpr std::size_t kWorldCapacity = 4;
inline constexpr std::uint16_t kInvalidWorldSlot = static_cast<std::uint16_t>(kWorldCapacity);
inline constexpr std::size_t kCheckpointRequestCapacity = 16;
inline constexpr std::size_t kIncidentAllowlistCapacity = 32;
inline constexpr std::size_t kTickStageCapacity = 10;
inline constexpr std::size_t kHostPeerCapacity = interest::kPeerCapacity;
inline constexpr std::uint32_t kScriptlessServiceBudget = 8;

/** Generation-checked reference to one active host world. */
struct WorldHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{kInvalidWorldSlot};

    /** @return True when both handles name the same host-world generation. */
    [[nodiscard]] constexpr bool operator==(const WorldHandle&) const noexcept = default;
};

/** Generation-checked reference to one host-owned peer service lifetime. */
struct HostPeerHandle final {
    std::uint64_t generation{};
    std::uint16_t slot{static_cast<std::uint16_t>(kHostPeerCapacity)};

    /** @return True when both handles name the same host-peer generation. */
    [[nodiscard]] constexpr bool operator==(const HostPeerHandle&) const noexcept = default;
};

/** Host-level result before a subsystem-specific result is available. */
enum class HostStatus : std::uint8_t {
    success,
    invalidArgument,
    unavailable,
    staleHandle,
    staleOwner,
    alreadyOpen,
    capacity,
    policyRejected,
    backendRejected,
    serviceRejected,
    unhealthy,
};

/** Exact resources copied without granting mutable registry access. */
struct WorldResources final {
    world::WorldIdentity identity{};
    backend::SceneHandle physicsScene{};
    backend::NavigationSceneHandle navigationScene{};
    backend::PhysicsCapabilities physics{};
    backend::NavigationCapabilities navigation{};
    std::uint64_t logicalWorldId{};
    std::uint64_t journalCursor{};
};

/** Exact inputs used to bind generic per-peer synchronization services. */
struct PeerOpenRequest final {
    interest::PeerKey interest{};
    common::Binding common{};
    state::gameplay::physics::PeerReplicaHandle replica{};
};

/** Borrowed world services valid only for the matching active world lifetime. */
struct WorldServiceAccess final {
    interest::InterestManager* interest{};
    authority::MotionValidator* motion{};
    backend::PhysicsBackend* physics{};
    backend::NavigationBackend* navigation{};
    controller::ActorControllerService* controller{};
};

/** Borrowed peer services valid only for the matching active peer lifetime. */
struct PeerServiceAccess final {
    common::State* common{};
    replication::PeerState* replication{};
};

/** One immutable checkpoint request after its world tick commits. */
struct CheckpointRequest final {
    world::WorldIdentity identity{};
    WorldHandle world{};
    world::TickId tick{};
    std::uint64_t canonicalHash{};
    std::uint64_t reasonId{};
    std::uint64_t logicalWorldId{};
    std::uint64_t journalCursor{};
};

/** Optional durable queue. Returning false selects the host fallback queue. */
class ICheckpointSink {
public:
    virtual ~ICheckpointSink() = default;
    /** @return True when the request entered durable caller-owned storage. */
    [[nodiscard]] virtual bool request_checkpoint(const CheckpointRequest& request) noexcept = 0;
};

/** Generic controller seam with no transport or wire access. */
class IControllerExtension {
public:
    virtual ~IControllerExtension() = default;

    /** @return True after saving controller state for one exact host tick. */
    [[nodiscard]] virtual bool begin_tick(const world::WorldIdentity& identity,
                                          world::TickId tick) noexcept {
        static_cast<void>(identity);
        static_cast<void>(tick);
        return false;
    }
    /** Commits controller state saved for one successful host tick. */
    virtual void commit_tick(const world::WorldIdentity& identity, world::TickId tick) noexcept {
        static_cast<void>(identity);
        static_cast<void>(tick);
    }
    /** Restores controller state after any failed host tick. */
    virtual void rollback_tick(const world::WorldIdentity& identity, world::TickId tick) noexcept {
        static_cast<void>(identity);
        static_cast<void>(tick);
    }

    /** @return Result for one ordered controller configuration command. */
    [[nodiscard]] virtual world::HostCommandExecutionResult
    configure(const world::HostExecutionContext& context,
              const world::HostCommand& command,
              std::span<const world::ActorSnapshot> actors,
              backend::PhysicsBackend& physics,
              backend::NavigationBackend& navigation,
              std::span<world::CommittedEvent> events) noexcept = 0;

    /** @return Bounded controller work for one fixed service tick. */
    [[nodiscard]] virtual world::HostTickExecutionResult
    step(const world::HostExecutionContext& context,
         std::span<const world::ActorSnapshot> actors,
         std::span<const world::CommittedEvent> committedEvents,
         backend::PhysicsBackend& physics,
         backend::NavigationBackend& navigation,
         std::span<world::CommittedEvent> events) noexcept = 0;
};

/** Inputs fixed for one BubbleHost world lifetime. */
struct WorldOpenRequest final {
    backend::SceneSpec scene{};
    ICheckpointSink* checkpointSink{};
    IControllerExtension* controller{};
    std::uint64_t logicalWorldId{};
    std::uint64_t activitySessionId{};
    std::uint64_t ownerEpoch{};
    std::uint64_t deterministicSeed{};
    float millimetersPerUnit{};
    std::uint32_t scriptlessTriggerBudget{kScriptlessServiceBudget};
    std::uint32_t scriptlessQueryBudget{kScriptlessServiceBudget};
    std::uint32_t scriptlessCounterBudget{kScriptlessServiceBudget};
    std::uint32_t scriptlessCreditBudget{kScriptlessServiceBudget};
    bool allowHostActorCommands{};
};

/** Ordered host stages visible to tests and scheduler telemetry. */
enum class TickStage : std::uint8_t {
    logicalCommit,
    actorSync,
    controllerNavigation,
    physics,
    combat,
    triggers,
    objectiveCredit,
    timers,
    policyPostTick,
    checkpointDispatch,
};

/** One fixed-tick result without backend-owned pointers. */
struct TickResult final {
    std::array<TickStage, kTickStageCapacity> stages{};
    world::AdvanceResult world{};
    HostStatus status{HostStatus::staleHandle};
    std::size_t stageCount{};
    std::size_t contactCount{};
    std::size_t triggerEventCount{};
    std::size_t timerEventCount{};
    std::size_t checkpointCount{};
};

/** Host and queue results for one fully stamped command. */
struct CommandSubmitResult final {
    HostStatus status{HostStatus::staleHandle};
    world::CommandSubmitStatus command{world::CommandSubmitStatus::invalid};
};

/** Build-data combat profile selected by an activity command. */
struct CombatProfile final {
    world::BlueprintRef profile{};
    std::uint64_t policyOwnerId{};
    std::uint64_t hostileFactionMask{};
    std::uint32_t maximumHealth{};
    std::uint32_t maximumShield{};
    std::uint8_t faction{};
    bool canDealDamage{};
    bool canReceiveDamage{};
};

/** Build-data attack profile used to validate damage intents. */
struct AttackProfile final {
    world::BlueprintRef attack{};
    std::uint64_t policyOwnerId{};
    std::uint32_t damage{};
    std::uint32_t maximumRangeMillimeters{};
    world::TickId cooldownTicks{};
};

/** Logical generic-service state. Backend handles are intentionally absent. */
struct ServiceSnapshot final {
    controller::ControllerServiceSnapshot controllers{};
    mechanics::TickTimerServiceSnapshot timers{};
    mechanics::TriggerSystemSnapshot triggers{};
    mechanics::ObjectiveLedgerSnapshot objectives{};
    mechanics::ParticipantCreditLedgerSnapshot credits{};
    mechanics::CombatKernelSnapshot combat{};
};

} // namespace sunrise::server::gameplay::physics::host
