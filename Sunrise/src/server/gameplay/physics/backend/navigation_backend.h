#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "physics_backend.h"

namespace sunrise::server::gameplay::physics::backend {

/** A generation-checked navigation scene reference. */
struct NavigationSceneHandle {
    std::uint16_t slot{kInvalidSlot};
    std::uint32_t generation{};

    /** @return True when both handles name the same navigation-scene generation. */
    [[nodiscard]] constexpr bool operator==(const NavigationSceneHandle&) const noexcept = default;
};

/** Scriptless navigation binds one physics scene and content build. */
struct NavigationSceneSpec {
    std::uint64_t stableSceneId{};
    std::uint64_t contentBuild{};
    SceneHandle physicsScene{};
    std::uint32_t bubble{};
};

/** One position tied to an exact navigation scene and bubble. */
struct NavigationPoint {
    NavigationSceneHandle scene{};
    std::uint32_t bubble{};
    Vec3 position{};
};

/** Explicit result of a scriptless navigation request. */
enum class NavigationStatus : std::uint8_t {
    success,
    invalidQuery,
    staleScene,
    contextMismatch,
    outsideBounds,
    blocked,
};

/** One collision proxy used to validate a navigation request. */
struct NavigationAgent {
    std::uint64_t stableOwnerId{};
    std::uint32_t shapeId{};
    CollisionGate gate{};
    Shape shape{};
    std::uint64_t ignoreOwnerId{};
};

/** Point projection input for the scriptless fallback. */
struct ProjectPointQuery {
    NavigationPoint point{};
    NavigationAgent agent{};
};

/** Straight-line reachability input. */
struct ReachabilityQuery {
    NavigationPoint start{};
    NavigationPoint goal{};
    NavigationAgent agent{};
};

/** A bounded path which never owns heap storage. */
struct NavigationPath {
    std::array<NavigationPoint, kPathPointCapacity> points{};
    std::size_t count{};
};

/** Capabilities exposed to activity manifests. */
struct NavigationCapabilities {
    bool projectPoint{true};
    bool reachable{true};
    bool straightLinePath{true};
    bool meshPath{};
};

/** A safe scriptless navigation backend over physics queries. */
class NavigationBackend final {
public:
    /** Binds fixed navigation storage to one physics backend. */
    void initialize(const PhysicsBackend& physics) noexcept;

    /** Clears navigation storage and invalidates earlier handles. */
    void shutdown() noexcept;

    /** Returns the scriptless fallback capability set. */
    [[nodiscard]] NavigationCapabilities capabilities() const noexcept;

    /** Loads one build- and bubble-matched scriptless scene. */
    [[nodiscard]] bool load_scene(const NavigationSceneSpec& spec,
                                  NavigationSceneHandle& handle) noexcept;

    /** Unloads one exact navigation scene. */
    [[nodiscard]] bool unload_scene(NavigationSceneHandle handle) noexcept;

    /** Keeps a point only when its exact pose is inside and collision-free. */
    [[nodiscard]] NavigationStatus project_point(const ProjectPointQuery& query,
                                                 NavigationPoint& projected) const noexcept;

    /** Accepts only a collision-free segment in one exact scene and bubble. */
    [[nodiscard]] NavigationStatus reachable(const ReachabilityQuery& query) const noexcept;

    /** Returns only a safe straight-line path, or an explicit failure. */
    [[nodiscard]] NavigationStatus find_path(const ReachabilityQuery& query,
                                             NavigationPath& path) const noexcept;

private:
    struct SceneSlot {
        NavigationSceneSpec spec{};
        Aabb bounds{};
        std::uint32_t generation{1};
        bool occupied{};
    };

    std::array<SceneSlot, kSceneCapacity> scenes_{};
    const PhysicsBackend* physics_{};
    bool initialized_{};
};

} // namespace sunrise::server::gameplay::physics::backend
