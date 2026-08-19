#pragma once

#include <array>
#include <cstdint>

#include "types.h"

namespace sunrise::server::gameplay::physics::backend {

/** A fixed-capacity deterministic physics backend. */
class PhysicsBackend final {
public:
    /** Clears fixed storage and invalidates earlier handles. */
    void initialize() noexcept;

    /** Clears fixed storage and invalidates earlier handles. */
    void shutdown() noexcept;

    /** Returns capabilities which do not depend on loaded content. */
    [[nodiscard]] PhysicsCapabilities capabilities() const noexcept;

    /** Loads one validated build-bound scene into a fixed slot. */
    [[nodiscard]] bool load_scene(const SceneSpec& spec, SceneHandle& handle) noexcept;

    /** Unloads one scene and invalidates all bodies owned by it. */
    [[nodiscard]] bool unload_scene(SceneHandle handle) noexcept;

    /** Copies one exact scene without exposing backend storage. */
    [[nodiscard]] bool read_scene(SceneHandle handle, SceneState& state) const noexcept;

    /** Creates one validated body in a fixed slot. */
    [[nodiscard]] bool create_body(const BodySpec& spec, BodyHandle& handle) noexcept;

    /** Destroys one exact body and invalidates its handle. */
    [[nodiscard]] bool destroy_body(BodyHandle handle) noexcept;

    /** Queues an exact pose for the next kinematic step. */
    [[nodiscard]] bool set_kinematic_target(BodyHandle handle, const Transform& target) noexcept;

    /** Moves one body immediately and clears its pending target. */
    [[nodiscard]] bool teleport_body(BodyHandle handle, const Transform& transform) noexcept;

    /** Replaces one movable body's canonical velocities. */
    [[nodiscard]] bool set_velocity(BodyHandle handle, Vec3 linear, Vec3 angular) noexcept;

    /** Adds one world-space impulse to a dynamic body. */
    [[nodiscard]] bool apply_impulse(BodyHandle handle, Vec3 impulse) noexcept;

    /** Advances every movable body by one finite fixed interval. */
    [[nodiscard]] bool step(float fixedDt) noexcept;

    /** Copies one exact body without exposing backend storage. */
    [[nodiscard]] bool read_body(BodyHandle handle, BodyState& state) const noexcept;

    /** Moves the last step's ordered contacts into caller-owned storage. */
    [[nodiscard]] bool drain_contacts(ContactBuffer& contacts) noexcept;

    /** Runs one ordered ray query without changing output on failure. */
    [[nodiscard]] QueryStatus raycast(const RayQuery& query, QueryHitBuffer& hits) const noexcept;

    /** Runs one ordered conservative sweep without changing output on failure. */
    [[nodiscard]] QueryStatus sweep(const SweepQuery& query, QueryHitBuffer& hits) const noexcept;

    /** Runs one ordered conservative overlap without changing output on failure. */
    [[nodiscard]] QueryStatus overlap(const OverlapQuery& query,
                                      QueryHitBuffer& hits) const noexcept;

    /** Returns the number of accepted fixed steps since initialization. */
    [[nodiscard]] std::uint64_t tick() const noexcept;

private:
    struct SceneSlot {
        SceneSpec spec{};
        std::uint32_t generation{1};
        std::size_t bodyCount{};
        bool occupied{};
    };

    struct BodySlot {
        BodySpec spec{};
        Transform previousTransform{};
        Transform kinematicTarget{};
        std::uint32_t generation{1};
        std::uint64_t revision{};
        bool occupied{};
        bool hasKinematicTarget{};
        bool kinematicVelocityDriven{};
    };

    std::array<SceneSlot, kSceneCapacity> scenes_{};
    std::array<BodySlot, kBodyCapacity> bodies_{};
    ContactBuffer contacts_{};
    std::uint64_t tick_{};
    float fixedDt_{};
    bool initialized_{};

    void discard_contacts(BodyHandle handle) noexcept;
    [[nodiscard]] bool prepare_step(float fixedDt,
                                    std::array<BodySlot, kBodyCapacity>& nextBodies) const noexcept;
    [[nodiscard]] bool build_contacts(const std::array<BodySlot, kBodyCapacity>& nextBodies,
                                      std::uint64_t nextTick,
                                      ContactBuffer& contacts) const noexcept;
    [[nodiscard]] bool build_contact(const BodySlot& first,
                                     const BodySlot& second,
                                     std::size_t firstSlot,
                                     std::size_t secondSlot,
                                     Contact& contact) const noexcept;
};

} // namespace sunrise::server::gameplay::physics::backend
