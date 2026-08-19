#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "controller_types.h"
#include "server/gameplay/physics/world/host_command.h"

namespace sunrise::server::gameplay::physics::controller {

/** Fixed-capacity steering and path mechanics for host-owned actors. */
class ActorControllerService final {
public:
    /** Binds the service to exact physics and navigation backends. */
    [[nodiscard]] bool initialize(backend::PhysicsBackend& physics,
                                  backend::NavigationBackend& navigation,
                                  float fixedDt) noexcept;

    /** Stops live controllers and invalidates every issued handle. */
    void shutdown() noexcept;

    /** Applies one typed controller command after host data resolution. */
    [[nodiscard]] ControllerOperationResult
    configure_controller(const world::ConfigureControllerCommand& command,
                         const ControllerBinding& binding,
                         const ControllerProfile& profile) noexcept;

    /** Resolves one typed path command through the bound navigation backend. */
    [[nodiscard]] ControllerOperationResult request_path(const world::RequestPathCommand& command,
                                                         world::TickId tick) noexcept;

    /** Stops and removes one exact controller generation. */
    [[nodiscard]] ControllerOperationResult remove_controller(ControllerHandle controller) noexcept;

    /** Finds the controller bound to one exact logical actor generation. */
    [[nodiscard]] bool find_controller(world::ActorKey actor,
                                       ControllerHandle& controller) const noexcept;

    /** Copies one exact controller without exposing service storage. */
    [[nodiscard]] bool read_controller(ControllerHandle controller,
                                       ControllerSnapshot& snapshot) const noexcept;

    /** Queues kinematic targets before the matching physics fixed step. */
    [[nodiscard]] ControllerStepResult step(world::TickId tick,
                                            std::span<const world::ActorSnapshot> actors) noexcept;

    /** Returns ordered outcomes retained until the host clears them. */
    [[nodiscard]] std::span<const ControllerEvent> events() const noexcept;

    /** Clears outcomes after the host commits or copies them. */
    void clear_events() noexcept;

    /** Copies logical controller state without event or transport storage. */
    [[nodiscard]] bool snapshot(ControllerServiceSnapshot& snapshot) const noexcept;

    /** Restores valid process-local bindings and issues fresh handles. */
    [[nodiscard]] ControllerOperationResult
    restore(const ControllerServiceSnapshot& snapshot) noexcept;

    /** @return Number of configured controllers in fixed storage. */
    [[nodiscard]] std::size_t controller_count() const noexcept;

    /** @return False after a bounded outcome buffer overflows. */
    [[nodiscard]] bool healthy() const noexcept;

private:
    struct Slot final {
        ControllerBinding binding{};
        ControllerProfile profile{};
        world::ActorKey targetActor{};
        world::Vector3 facingPosition{};
        backend::NavigationPath path{};
        backend::Vec3 commandedVelocity{};
        float lastWaypointDistance{};
        std::size_t waypointIndex{};
        std::uint32_t generation{1};
        std::uint64_t revision{};
        std::uint32_t stalledTicks{};
        ControllerState state{ControllerState::idle};
        bool occupied{};
        bool hasProgressSample{};
    };

    std::array<Slot, kControllerCapacity> slots_{};
    std::array<ControllerEvent, kControllerEventCapacity> events_{};
    backend::PhysicsBackend* physics_{};
    backend::NavigationBackend* navigation_{};
    world::TickId lastTick_{};
    std::uint64_t nextEventSequence_{1};
    float fixedDt_{};
    std::size_t controllerCount_{};
    std::size_t eventCount_{};
    bool initialized_{};
    bool healthy_{true};

    [[nodiscard]] bool append_event(std::size_t slot,
                                    world::TickId tick,
                                    ControllerEventKind kind,
                                    ControllerStatus status,
                                    backend::NavigationStatus navigationStatus,
                                    world::Vector3 position) noexcept;
    [[nodiscard]] bool stop_motion(Slot& slot) noexcept;
    void stop_with_state(Slot& slot, ControllerState state) noexcept;
    void step_controller(std::size_t slotIndex,
                         Slot& slot,
                         const backend::BodyState& body,
                         const world::ActorSnapshot* facingActor,
                         world::TickId tick,
                         ControllerStepResult& result) noexcept;
};

} // namespace sunrise::server::gameplay::physics::controller
