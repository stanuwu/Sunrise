#pragma once

#include <memory>
#include <span>

#include "types.h"

namespace sunrise::server::gameplay::physics::host {

/** Fixed owner for scriptless physics worlds. */
class BubbleHost final : private world::IHostCommandExecutor {
public:
    /** Opaque fixed registry storage used by the split implementation. */
    struct Storage;

    BubbleHost() noexcept;
    ~BubbleHost() override;
    BubbleHost(const BubbleHost&) = delete;
    BubbleHost& operator=(const BubbleHost&) = delete;

    /** @return True when fixed registry storage was allocated. */
    [[nodiscard]] bool ready() const noexcept;
    /** @return Number of active exact world lifetimes. */
    [[nodiscard]] std::size_t world_count() const noexcept;

    /** Opens one world transactionally. Null policy selects the empty fallback. */
    [[nodiscard]] HostStatus open_world(const WorldOpenRequest& request,
                                        world::IActivityPolicy* policy,
                                        WorldHandle& handle) noexcept;
    /** Closes one exact world and invalidates its handle before reuse. */
    [[nodiscard]] HostStatus close_world(WorldHandle handle) noexcept;
    /** Replaces one world with a newer empty lifetime. */
    [[nodiscard]] HostStatus restart_empty(WorldHandle handle,
                                           const WorldOpenRequest& request,
                                           WorldHandle& replacement) noexcept;

    /** Submits one validated stamped command. */
    [[nodiscard]] CommandSubmitResult submit(WorldHandle handle,
                                             const world::HostCommand& command) noexcept;
    /** Runs one fixed world tick through policy and generic services. */
    [[nodiscard]] TickResult tick(WorldHandle handle) noexcept;
    /** Copies one pointer-free logical world snapshot. */
    [[nodiscard]] HostStatus snapshot(WorldHandle handle,
                                      world::WorldSnapshot& output) const noexcept;
    /** Copies bounded writer metrics for one exact world lifetime. */
    [[nodiscard]] HostStatus world_metrics(WorldHandle handle,
                                           world::WorldMetrics& output) const noexcept;
    /** Copies scene handles and capabilities. */
    [[nodiscard]] HostStatus resources(WorldHandle handle, WorldResources& output) const noexcept;

    /** Binds one peer to host-owned sync state. */
    [[nodiscard]] HostStatus
    bind_peer(WorldHandle handle, const PeerOpenRequest& request, HostPeerHandle& output) noexcept;
    /** Releases one exact peer after its planner-owned references drain. */
    [[nodiscard]] HostStatus unbind_peer(WorldHandle handle, HostPeerHandle peer) noexcept;
    /** Borrows single-writer world services. */
    [[nodiscard]] HostStatus world_services(WorldHandle handle,
                                            WorldServiceAccess& output) noexcept;
    /** Borrows single-writer peer services. */
    [[nodiscard]] HostStatus
    peer_services(WorldHandle handle, HostPeerHandle peer, PeerServiceAccess& output) noexcept;
    /** @return Host-owned fixed checkpoint store, or null when unavailable. */
    [[nodiscard]] persistence::CheckpointStore* checkpoint_store() noexcept;
    /** @return Immutable host-owned checkpoint store, or null when unavailable. */
    [[nodiscard]] const persistence::CheckpointStore* checkpoint_store() const noexcept;
    /** @return Host-owned durable command journal, or null when unavailable. */
    [[nodiscard]] persistence::CommandJournal* command_journal() noexcept;
    /** @return Immutable durable command journal, or null when unavailable. */
    [[nodiscard]] const persistence::CommandJournal* command_journal() const noexcept;

    /** Binds a body for canonical pose return. */
    [[nodiscard]] HostStatus bind_actor_body(WorldHandle handle,
                                             world::ActorKey actor,
                                             const backend::BodySpec& spec,
                                             backend::BodyHandle& body) noexcept;
    /** Removes one exact actor body binding. */
    [[nodiscard]] HostStatus unbind_actor_body(WorldHandle handle, world::ActorKey actor) noexcept;
    /** Copies one exact bound body. */
    [[nodiscard]] HostStatus read_actor_body(WorldHandle handle,
                                             world::ActorKey actor,
                                             backend::BodyState& body) const noexcept;

    /** Runs one safe scriptless point projection. */
    [[nodiscard]] HostStatus project_point(WorldHandle handle,
                                           const backend::ProjectPointQuery& query,
                                           backend::NavigationPoint& point,
                                           backend::NavigationStatus& status) const noexcept;
    /** Runs one safe scriptless path query. */
    [[nodiscard]] HostStatus find_path(WorldHandle handle,
                                       const backend::ReachabilityQuery& query,
                                       backend::NavigationPath& path,
                                       backend::NavigationStatus& status) const noexcept;

    /** Adds one immutable combat profile for later policy selection. */
    [[nodiscard]] HostStatus register_combat_profile(WorldHandle handle,
                                                     const CombatProfile& profile) noexcept;
    /** Adds one immutable attack profile. */
    [[nodiscard]] HostStatus register_attack_profile(WorldHandle handle,
                                                     const AttackProfile& profile) noexcept;
    /** Adds one immutable generic steering profile. */
    [[nodiscard]] HostStatus
    register_controller_profile(WorldHandle handle,
                                const controller::ControllerProfile& profile) noexcept;
    /** Adds one generic objective definition. */
    [[nodiscard]] HostStatus
    create_objective(WorldHandle handle,
                     const mechanics::ObjectiveCounterDefinition& definition,
                     mechanics::ObjectiveCounterHandle& counter) noexcept;
    /** Adds one participant credit definition. */
    [[nodiscard]] HostStatus
    create_participant(WorldHandle handle,
                       const mechanics::ParticipantCreditDefinition& definition,
                       mechanics::ParticipantCreditHandle& participant) noexcept;
    /** Adds one presentation incident id to the fail-closed world allowlist. */
    [[nodiscard]] HostStatus allow_incident(WorldHandle handle, std::uint64_t incidentId) noexcept;

    /** Copies logical generic-service state for persistence. */
    [[nodiscard]] HostStatus snapshot_services(WorldHandle handle,
                                               ServiceSnapshot& output) const noexcept;
    /** Restores services into a fresh world epoch. */
    [[nodiscard]] HostStatus restore_services(WorldHandle handle,
                                              const ServiceSnapshot& source) noexcept;
    /** Expires replay identities only below a caller-confirmed durable floor. */
    [[nodiscard]] HostStatus expire_commands_before(WorldHandle handle,
                                                    world::TickId firstRetainedTick) noexcept;

    /** Moves queued fallback checkpoint requests into caller-owned storage. */
    [[nodiscard]] HostStatus drain_checkpoint_requests(WorldHandle handle,
                                                       std::span<CheckpointRequest> requests,
                                                       std::size_t& requestCount) noexcept;
    /** Copies the last fixed tick's drained physics contacts. */
    [[nodiscard]] HostStatus copy_last_contacts(WorldHandle handle,
                                                backend::ContactBuffer& contacts) const noexcept;
    /** Copies the last fixed tick's trigger events. */
    [[nodiscard]] HostStatus copy_last_trigger_events(WorldHandle handle,
                                                      std::span<mechanics::TriggerEvent> events,
                                                      std::size_t& eventCount) const noexcept;
    /** Copies the last fixed tick's timer events. */
    [[nodiscard]] HostStatus copy_last_timer_events(WorldHandle handle,
                                                    std::span<mechanics::TickTimerEvent> events,
                                                    std::size_t& eventCount) const noexcept;
    /** Copies the latest path retained for one actor generation. */
    [[nodiscard]] HostStatus read_path(WorldHandle handle,
                                       world::ActorKey actor,
                                       backend::NavigationPath& path) const noexcept;

private:
    friend struct HostCommandProcessor;
    friend struct HostTickProcessor;

    [[nodiscard]] std::size_t find_slot(WorldHandle handle) const noexcept;
    [[nodiscard]] std::size_t find_world(const world::WorldIdentity& identity) const noexcept;
    [[nodiscard]] std::size_t find_peer(std::size_t worldSlot,
                                        HostPeerHandle handle) const noexcept;
    void record_stage(std::size_t slot, TickStage stage) noexcept;

    [[nodiscard]] world::HostCommandExecutionResult
    execute_command(const world::HostExecutionContext& context,
                    const world::HostCommand& command,
                    std::span<const world::ActorSnapshot> actors,
                    std::span<world::CommittedEvent> events) noexcept override;
    [[nodiscard]] world::HostTickExecutionResult
    step_services(const world::HostExecutionContext& context,
                  std::span<const world::ActorSnapshot> actors,
                  std::span<const world::CommittedEvent> committedEvents,
                  std::span<world::CommittedEvent> events) noexcept override;

    std::unique_ptr<Storage> storage_{};
};

} // namespace sunrise::server::gameplay::physics::host
