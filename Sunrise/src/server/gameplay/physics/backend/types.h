#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::server::gameplay::physics::backend {

/** Fixed scenes keep world and public-region contexts disjoint. */
inline constexpr std::size_t kSceneCapacity = 16;
/** The first host envelope allows at most 256 server actors. */
inline constexpr std::size_t kBodyCapacity = 256;
/** One query can return every fixed body once. */
inline constexpr std::size_t kQueryHitCapacity = kBodyCapacity;
/** Contact overload keeps the first 1,024 stable-order pairs. */
inline constexpr std::size_t kContactCapacity = 1'024;
/** Activity policy paths have eight fixed output points. */
inline constexpr std::size_t kPathPointCapacity = 8;
/** The invalid slot is outside every fixed backend capacity. */
inline constexpr std::uint16_t kInvalidSlot = 0xFFFFU;
/** Exhausted generations retire their fixed slot permanently. */
inline constexpr std::uint32_t kExhaustedGeneration = 0xFFFFFFFFU;
/** Collision masks contain exactly 32 addressable layers. */
inline constexpr std::uint8_t kCollisionLayerCount = 32;
/** This mask admits every addressable collision layer. */
inline constexpr std::uint32_t kAllCollisionLayers = 0xFFFFFFFFU;

/** A host-space vector in canonical scene units. */
struct Vec3 {
    float x{};
    float y{};
    float z{};
};

/** A host-space rotation stored as an x/y/z/w quaternion. */
struct Quaternion {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

/** A canonical body pose. */
struct Transform {
    Vec3 position{};
    Quaternion rotation{};
};

/** An axis-aligned scene bound. */
struct Aabb {
    Vec3 minimum{};
    Vec3 maximum{};
};

/** A generation-checked physics scene reference. */
struct SceneHandle {
    std::uint16_t slot{kInvalidSlot};
    std::uint32_t generation{};

    /** @return True when both handles name the same physics-scene generation. */
    [[nodiscard]] constexpr bool operator==(const SceneHandle&) const noexcept = default;
};

/** A generation-checked body reference. */
struct BodyHandle {
    std::uint16_t slot{kInvalidSlot};
    std::uint32_t generation{};

    /** @return True when both handles name the same physics-body generation. */
    [[nodiscard]] constexpr bool operator==(const BodyHandle&) const noexcept = default;
};

/** Conservative proxy kinds supported by the first backend. */
enum class ShapeKind : std::uint8_t {
    sphere,
    box,
    capsule,
};

/** One conservative collision proxy. */
struct Shape {
    ShapeKind kind{ShapeKind::sphere};
    float radius{};
    float halfHeight{};
    Vec3 halfExtents{};
};

/** A symmetric layer/filter and bubble gate. */
struct CollisionGate {
    std::uint32_t bubble{};
    std::uint8_t layer{};
    std::uint32_t filterMask{kAllCollisionLayers};
};

/** Motion ownership used by the fixed-step integrator. */
enum class MotionKind : std::uint8_t {
    staticBody,
    kinematic,
    dynamic,
    count,
};

/** Build-bound scene input. */
struct SceneSpec {
    std::uint64_t stableSceneId{};
    std::uint64_t contentBuild{};
    float coordinateScale{1.0F};
    Vec3 origin{};
    std::uint32_t bubble{};
    std::uint32_t materialSetId{};
    std::uint32_t filterSetId{};
    Aabb bounds{};
};

/** Stored scene state without backend-owned pointers. */
struct SceneState {
    SceneHandle handle{};
    SceneSpec spec{};
    std::size_t bodyCount{};
};

/** One body creation request. */
struct BodySpec {
    SceneHandle scene{};
    std::uint64_t stableOwnerId{};
    std::uint32_t shapeId{};
    CollisionGate gate{};
    Shape shape{};
    Transform transform{};
    Vec3 linearVelocity{};
    Vec3 angularVelocity{};
    float inverseMass{};
    MotionKind motion{MotionKind::staticBody};
    bool isTrigger{};
};

/** Canonical body state copied out of fixed storage. */
struct BodyState {
    BodyHandle handle{};
    BodySpec spec{};
    Transform previousTransform{};
    Transform kinematicTarget{};
    std::uint64_t revision{};
    bool hasKinematicTarget{};
};

/** Query completion status. */
enum class QueryStatus : std::uint8_t {
    success,
    invalidQuery,
    staleScene,
    contextMismatch,
};

/** One deterministic query result. */
struct QueryHit {
    BodyHandle body{};
    std::uint64_t stableOwnerId{};
    std::uint32_t shapeId{};
    float fraction{};
    float distance{};
    Vec3 position{};
    Vec3 normal{};
    bool isTrigger{};
};

/** Fixed query output ordered by time, owner, and shape. */
struct QueryHitBuffer {
    std::array<QueryHit, kQueryHitCapacity> hits{};
    std::size_t count{};
    bool truncated{};
};

/** A finite ray against one exact scene and bubble. */
struct RayQuery {
    SceneHandle scene{};
    std::uint64_t stableOwnerId{};
    std::uint32_t shapeId{};
    CollisionGate gate{};
    Vec3 origin{};
    Vec3 direction{};
    float maxDistance{};
    std::uint64_t ignoreOwnerId{};
    bool includeTriggers{};
};

/** A conservative shape sweep over one displacement. */
struct SweepQuery {
    SceneHandle scene{};
    std::uint64_t stableOwnerId{};
    std::uint32_t shapeId{};
    CollisionGate gate{};
    Shape shape{};
    Transform transform{};
    Vec3 displacement{};
    std::uint64_t ignoreOwnerId{};
    bool includeTriggers{};
};

/** A conservative shape overlap at one pose. */
struct OverlapQuery {
    SceneHandle scene{};
    std::uint64_t stableOwnerId{};
    std::uint32_t shapeId{};
    CollisionGate gate{};
    Shape shape{};
    Transform transform{};
    std::uint64_t ignoreOwnerId{};
    bool includeTriggers{};
};

/** One ordered contact pair from the last fixed step. */
struct Contact {
    BodyHandle firstBody{};
    BodyHandle secondBody{};
    std::uint64_t firstOwnerId{};
    std::uint64_t secondOwnerId{};
    std::uint32_t firstShapeId{};
    std::uint32_t secondShapeId{};
    float fraction{};
    Vec3 point{};
    Vec3 normal{};
    bool isTrigger{};
};

/** Fixed contact output ordered by time and stable identity. */
struct ContactBuffer {
    std::array<Contact, kContactCapacity> contacts{};
    std::size_t count{};
    std::size_t dropped{};
    std::uint64_t tick{};
};

/** Capabilities exposed to activity manifests. */
struct PhysicsCapabilities {
    bool conservativeProxies{true};
    bool kinematicMotion{true};
    bool dynamicMotion{true};
    bool rayQueries{true};
    bool sweepQueries{true};
    bool overlapQueries{true};
    bool deterministicContacts{true};
    bool collisionResponse{};
};

} // namespace sunrise::server::gameplay::physics::backend
