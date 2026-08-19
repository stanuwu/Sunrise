#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "server/gameplay/physics/backend/navigation_backend.h"
#include "server/gameplay/physics/world/world_types.h"

namespace sunrise::server::gameplay::physics::controller {

namespace backend = server::gameplay::physics::backend;
namespace world = server::gameplay::physics::world;

/** Controller storage matches the first logical actor envelope. */
inline constexpr std::size_t kControllerCapacity = world::kActorCapacity;
/** One tick can publish stuck and repath events for every controller. */
inline constexpr std::size_t kControllerEventCapacity = kControllerCapacity * 2U;
/** Controller handles use the same invalid slot sentinel as backend handles. */
inline constexpr std::uint16_t kInvalidControllerSlot = backend::kInvalidSlot;
/** Snapshot version one stores only generic controller state. */
inline constexpr std::uint32_t kControllerSnapshotVersion = 1;

/** A generation-checked reference to one configured controller. */
struct ControllerHandle final {
    std::uint16_t slot{kInvalidControllerSlot};
    std::uint32_t generation{};

    /** @return True when slot and generation match. */
    [[nodiscard]] constexpr bool operator==(const ControllerHandle&) const noexcept = default;
};

/** Generic facing inputs selected by versioned controller data. */
enum class FacingMode : std::uint8_t {
    none,
    velocity,
    position,
    actor,
};

/** Versioned limits used by deterministic steering. */
struct ControllerProfile final {
    world::BlueprintRef controller{};
    world::BlueprintRef navigationProfile{};
    float maxSpeed{};
    float maxAcceleration{};
    float maxTurnRadiansPerSecond{};
    float arrivalRadius{};
    float waypointRadius{};
    float progressEpsilon{};
    std::uint32_t stuckTickLimit{};
    FacingMode facing{FacingMode::none};
    bool requestRepathWhenStuck{true};
};

/** Exact logical, physics, navigation, and bubble binding for one actor. */
struct ControllerBinding final {
    world::ActorKey actor{};
    backend::BodyHandle body{};
    backend::NavigationSceneHandle navigationScene{};
    backend::NavigationAgent navigationAgent{};
    std::uint32_t bubble{};
};

/** Stable controller phases exposed to policy and persistence. */
enum class ControllerState : std::uint8_t {
    idle,
    followingPath,
    arrived,
    pathFailed,
    stuck,
    actorStale,
    targetStale,
    contextMismatch,
};

/** Bounded operation results for BubbleHost command mapping. */
enum class ControllerStatus : std::uint8_t {
    success,
    notInitialized,
    invalid,
    full,
    notFound,
    staleHandle,
    staleActor,
    staleBody,
    contextMismatch,
    unsupportedBody,
    profileMismatch,
    navigationFailed,
    late,
    unhealthy,
};

/** Events contain mechanics outcomes, never activity decisions. */
enum class ControllerEventKind : std::uint8_t {
    pathComplete,
    pathFailed,
    stuck,
    repathRequested,
    actorStale,
    targetStale,
    contextMismatch,
};

/** One ordered outcome from a command or fixed controller tick. */
struct ControllerEvent final {
    ControllerHandle controller{};
    world::ActorKey actor{};
    world::TickId tick{};
    std::uint64_t sequence{};
    world::Vector3 position{};
    backend::NavigationStatus navigationStatus{backend::NavigationStatus::success};
    ControllerStatus status{ControllerStatus::success};
    ControllerEventKind kind{ControllerEventKind::pathComplete};
};

/** Result returned by configure, path, remove, and restore operations. */
struct ControllerOperationResult final {
    ControllerHandle controller{};
    backend::NavigationStatus navigationStatus{backend::NavigationStatus::success};
    ControllerStatus status{ControllerStatus::success};

    /** @return True only when the requested mutation committed. */
    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return status == ControllerStatus::success;
    }
};

/** Immutable state for one configured controller. */
struct ControllerSnapshot final {
    ControllerHandle handle{};
    ControllerBinding binding{};
    ControllerProfile profile{};
    world::ActorKey targetActor{};
    world::Vector3 facingPosition{};
    backend::NavigationPath path{};
    backend::Vec3 commandedVelocity{};
    float lastWaypointDistance{};
    std::size_t waypointIndex{};
    std::uint64_t revision{};
    std::uint32_t stalledTicks{};
    ControllerState state{ControllerState::idle};
    bool hasProgressSample{};
};

/** Pointer-free process-local checkpoint for the generic controller service. */
struct ControllerServiceSnapshot final {
    std::array<ControllerSnapshot, kControllerCapacity> controllers{};
    world::TickId lastTick{};
    std::uint64_t nextEventSequence{1};
    float fixedDt{};
    std::size_t controllerCount{};
    std::uint32_t version{kControllerSnapshotVersion};
    bool healthy{true};
};

/** Bounded summary of one deterministic service tick. */
struct ControllerStepResult final {
    std::size_t controllersProcessed{};
    std::size_t targetsQueued{};
    std::size_t eventsPublished{};
    bool healthy{true};
};

} // namespace sunrise::server::gameplay::physics::controller
