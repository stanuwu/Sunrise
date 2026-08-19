#pragma once

#include <array>
#include <limits>

#include "bubble_host.h"
#include "fallback_policy.h"

namespace sunrise::server::gameplay::physics::host {

struct BubbleHost::Storage final {
    struct BodyBinding final {
        world::ActorKey actor{};
        backend::BodyHandle body{};
        bool occupied{};
    };

    struct TriggerBinding final {
        mechanics::TriggerHandle trigger{};
        backend::BodyHandle body{};
        std::uint64_t triggerId{};
        bool occupied{};
    };

    struct CombatProfileSlot final {
        CombatProfile profile{};
        bool occupied{};
    };

    struct ControllerProfileSlot final {
        controller::ControllerProfile profile{};
        bool occupied{};
    };

    struct AttackProfileSlot final {
        AttackProfile profile{};
        mechanics::AttackBlueprintHandle attack{};
        bool occupied{};
    };

    struct CombatantBinding final {
        world::ActorKey actor{};
        world::BlueprintRef profile{};
        mechanics::CombatantHandle combatant{};
        bool occupied{};
    };

    struct PathBinding final {
        world::ActorKey actor{};
        backend::NavigationPath path{};
        bool occupied{};
    };

    struct PeerSlot final {
        common::State common{};
        replication::PeerState replication{};
        interest::PeerKey interestKey{};
        std::uint64_t generation{1};
        bool active{};
        bool retired{};
    };

    struct TickBackup final {
        backend::PhysicsBackend physics{};
        mechanics::TickTimerService timers{};
        mechanics::TriggerSystem triggers{};
        mechanics::ObjectiveLedger objectives{};
        mechanics::ParticipantCreditLedger credits{};
        mechanics::CombatKernel combat{};
        controller::ActorControllerService builtInController{};
        authority::MotionValidator motion{};
        std::array<BodyBinding, backend::kBodyCapacity> bodies{};
        std::array<TriggerBinding, mechanics::kTriggerCapacity> triggerBindings{};
        std::array<CombatantBinding, mechanics::kCombatantCapacity> combatants{};
        std::array<PathBinding, world::kActorCapacity> paths{};
        std::array<mechanics::TriggerEvent, mechanics::kTriggerMembershipCapacity * 2>
            triggerEvents{};
        std::array<mechanics::TickTimerEvent, mechanics::kTickTimerCapacity> timerEvents{};
        std::array<world::CanonicalPoseCommit, backend::kBodyCapacity> poseCommits{};
        backend::ContactBuffer contacts{};
        world::TickId serviceCommandTick{};
        std::size_t queryCommandCount{};
        std::size_t timerCommandCount{};
        std::size_t triggerCommandCount{};
        std::size_t objectiveCommandCount{};
        std::size_t creditCommandCount{};
        std::size_t combatCommandCount{};
        std::size_t triggerEventCount{};
        std::size_t timerEventCount{};
        std::size_t poseCommitCount{};
        bool healthy{};
        bool valid{};
    };

    struct WorldSlot final {
        world::WorldRunner runner{};
        ScriptlessPolicy fallbackPolicy{};
        backend::PhysicsBackend physics{};
        backend::NavigationBackend navigation{};
        mechanics::TickTimerService timers{};
        mechanics::TriggerSystem triggers{};
        mechanics::ObjectiveLedger objectives{};
        mechanics::ParticipantCreditLedger credits{};
        mechanics::CombatKernel combat{};
        controller::ActorControllerService builtInController{};
        interest::InterestManager interest{};
        authority::MotionValidator motion{};
        std::array<BodyBinding, backend::kBodyCapacity> bodies{};
        std::array<TriggerBinding, mechanics::kTriggerCapacity> triggerBindings{};
        std::array<CombatProfileSlot, mechanics::kCombatantCapacity> combatProfiles{};
        std::array<ControllerProfileSlot, controller::kControllerCapacity> controllerProfiles{};
        std::array<AttackProfileSlot, mechanics::kAttackBlueprintCapacity> attackProfiles{};
        std::array<CombatantBinding, mechanics::kCombatantCapacity> combatants{};
        std::array<PathBinding, world::kActorCapacity> paths{};
        std::array<PeerSlot, kHostPeerCapacity> peers{};
        std::array<world::CanonicalPoseCommit, backend::kBodyCapacity> poseCommits{};
        TickBackup tickBackup{};
        std::array<mechanics::TriggerEvent, mechanics::kTriggerMembershipCapacity * 2>
            triggerEvents{};
        std::array<mechanics::TickTimerEvent, mechanics::kTickTimerCapacity> timerEvents{};
        std::array<CheckpointRequest, kCheckpointRequestCapacity> checkpointRequests{};
        std::array<std::uint64_t, kIncidentAllowlistCapacity> incidentAllowlist{};
        std::array<TickStage, kTickStageCapacity> tickStages{};
        backend::ContactBuffer contacts{};
        backend::SceneHandle physicsScene{};
        backend::NavigationSceneHandle navigationScene{};
        ICheckpointSink* checkpointSink{};
        IControllerExtension* controller{};
        std::uint64_t logicalWorldId{};
        std::uint64_t journalCursor{};
        std::uint64_t handleGeneration{1};
        std::uint64_t lastOwnerEpoch{};
        world::TickId serviceCommandTick{};
        std::size_t queryCommandCount{};
        std::size_t timerCommandCount{};
        std::size_t triggerCommandCount{};
        std::size_t objectiveCommandCount{};
        std::size_t creditCommandCount{};
        std::size_t combatCommandCount{};
        std::size_t poseCommitCount{};
        std::size_t triggerEventCount{};
        std::size_t timerEventCount{};
        std::size_t checkpointRequestCount{};
        std::size_t incidentCount{};
        std::size_t tickStageCount{};
        std::uint32_t triggerBudget{};
        std::uint32_t queryBudget{};
        std::uint32_t counterBudget{};
        std::uint32_t creditBudget{};
        float millimetersPerUnit{};
        bool active{};
        bool healthy{};
        bool inTick{};
        bool retired{};
    };

    struct WorldCollection final {
        std::array<std::unique_ptr<WorldSlot>, kWorldCapacity> slots{};

        /** @return Fixed registry capacity. */
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return slots.size();
        }
        /** @return Allocated slot at one already validated index. */
        [[nodiscard]] WorldSlot& operator[](std::size_t index) noexcept {
            return *slots[index];
        }
        /** @return Immutable allocated slot at one already validated index. */
        [[nodiscard]] const WorldSlot& operator[](std::size_t index) const noexcept {
            return *slots[index];
        }
    };

    WorldCollection worlds{};
    persistence::CheckpointStore checkpoints{};
    std::unique_ptr<persistence::CommandJournal> journal{};
    /** Scratch for one service restore, so recovery allocates nothing on the calling thread. */
    ServiceSnapshot restoreScratch{};
    std::size_t worldCount{};
};

struct HostCommandProcessor final {
    [[nodiscard]] static world::HostCommandExecutionResult
    execute(BubbleHost& host,
            const world::HostExecutionContext& context,
            const world::HostCommand& command,
            std::span<const world::ActorSnapshot> actors,
            std::span<world::CommittedEvent> events) noexcept;
};

struct HostTickProcessor final {
    [[nodiscard]] static world::HostTickExecutionResult
    step(BubbleHost& host,
         const world::HostExecutionContext& context,
         std::span<const world::ActorSnapshot> actors,
         std::span<const world::CommittedEvent> committedEvents,
         std::span<world::CommittedEvent> events) noexcept;
};

namespace detail {

[[nodiscard]] world::HostCommandExecutionResult
validate_extension_command(const BubbleHost::Storage::WorldSlot& slot,
                           const world::HostExecutionContext& context,
                           const world::HostCommand& command,
                           std::span<const world::ActorSnapshot> actors) noexcept;

struct EventWriter final {
    std::span<world::CommittedEvent> events{};
    std::size_t count{};

    /** @return True when one copied event fit the runner-owned output. */
    [[nodiscard]] bool append(const world::CommittedEvent& event) noexcept;
    /** @return Writable output which begins after committed local events. */
    [[nodiscard]] std::span<world::CommittedEvent> remaining() const noexcept;
};

[[nodiscard]] bool reset_peer_services(BubbleHost::Storage::WorldSlot& slot) noexcept;
void capture_tick_backup(BubbleHost::Storage::WorldSlot& slot) noexcept;
[[nodiscard]] bool restore_tick_backup(BubbleHost::Storage::WorldSlot& slot) noexcept;

/** @return True when a handle can index active registry storage. */
[[nodiscard]] inline bool valid_handle_shape(WorldHandle handle) noexcept {
    return handle.slot < kWorldCapacity && handle.generation != 0;
}

/** @return Handle for one active slot index. */
[[nodiscard]] inline WorldHandle make_handle(std::size_t index, std::uint64_t generation) noexcept {
    return {generation, static_cast<std::uint16_t>(index)};
}

/** Advances a handle generation without permitting wrap aliases. */
inline void retire_handle_generation(std::uint64_t& generation, bool& retired) noexcept {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        retired = true;
        return;
    }
    ++generation;
}

} // namespace detail
} // namespace sunrise::server::gameplay::physics::host
