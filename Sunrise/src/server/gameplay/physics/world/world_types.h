#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::server::gameplay::physics::world {

using TickId = std::uint64_t;
using ActorId = std::uint64_t;
using PolicyOwnerId = std::uint64_t;
using CommandId = std::uint64_t;

/** Worlds use 30 fixed ticks per second unless a policy declares another fixed rate. */
inline constexpr std::uint32_t kDefaultFixedRateHz = 30;
/** Fixed rates above this bound are refused before a world opens. */
inline constexpr std::uint32_t kMaximumFixedRateHz = 120;
/** The first host envelope permits 256 simultaneous logical actors. */
inline constexpr std::size_t kActorCapacity = 256;
/** Fixed ingress storage bounds work accepted between writer ticks. */
inline constexpr std::size_t kCommandQueueCapacity = 512;
/** One admitted command can publish at most one committed event. */
inline constexpr std::size_t kCommittedEventCapacity = kCommandQueueCapacity;
/** Command replay storage retains 1,024 identities until a durable floor expires them. */
inline constexpr std::size_t kCommandHistoryCapacity = 1'024;
/** Policy rollback storage matches the durable policy-state bound. */
inline constexpr std::size_t kPolicyRollbackCapacity = 4'096;
/** One world retains 1,024 exact retired actor generations before draining. */
inline constexpr std::size_t kActorGenerationHistoryCapacity = 1'024;
/** A scheduler call runs at most four late ticks with unchanged fixed delta. */
inline constexpr std::uint32_t kMaximumCatchUpTicks = 4;
/** More than eight queued ticks makes the world unhealthy. */
inline constexpr std::uint32_t kMaximumTickDebt = 8;
/** New process-local generations start at one because zero is invalid. */
inline constexpr std::uint64_t kFirstGeneration = 1;
/** Fixed-step policy delta is always one tick. */
inline constexpr std::uint32_t kFixedDeltaNumerator = 1;

/** Exact activity, world lifetime, and current owner identity. */
struct WorldIdentity final {
    std::uint64_t activitySessionId{};
    std::uint64_t worldGeneration{};
    std::uint64_t ownerGeneration{};
};

/** Source classes have a stable global order inside one tick. */
enum class CommandSourceKind : std::uint8_t {
    host,
    network,
    policy,
    timer,
    count,
};

/** Stable source identity participates in deterministic command order. */
struct CommandSource final {
    std::uint64_t id{};
    CommandSourceKind kind{CommandSourceKind::host};
};

/** Stable logical actor id plus its independent reuse generation. */
struct ActorKey final {
    ActorId id{};
    std::uint64_t generation{};
};

/** Highest retired generation retained for one logical actor id. */
struct RetiredActorGeneration final {
    ActorId actorId{};
    std::uint64_t generation{};
    TickId retiredTick{};
};

/** Versioned host definition chosen by policy or build data. */
struct BlueprintRef final {
    std::uint64_t id{};
    std::uint32_t version{};
};

/** Full identity retained after a typed request commits. */
struct HostCommandHeader final {
    WorldIdentity world{};
    BlueprintRef definition{};
    CommandSource source{};
    CommandId commandId{};
    TickId executeTick{};
    PolicyOwnerId policyOwnerId{};
    std::uint64_t sourceSequence{};
};

/** Three finite canonical scalar coordinates. */
struct Vector3 final {
    float x{};
    float y{};
    float z{};
};

/** Finite nonzero rotation with the identity as its default. */
struct Quaternion final {
    float x{};
    float y{};
    float z{};
    float w{1.0F};
};

/** Canonical actor position and rotation. */
struct Transform final {
    Vector3 position{};
    Quaternion rotation{};
};

/** Host validation mode for actor motion. */
enum class MotionAuthorityMode : std::uint8_t {
    host,
    clientPredictedHostChecked,
    clientPresentational,
    count,
};

/** Motion ownership uses an epoch independent of actor and world reuse. */
struct MotionAuthority final {
    std::uint64_t ownerMemberId{};
    std::uint64_t generation{};
    TickId validFromTick{};
    MotionAuthorityMode mode{MotionAuthorityMode::host};
};

/** Logical actors are exposed only after their spawn commits. */
enum class ActorLifecycle : std::uint8_t {
    live,
};

/** Pointer-free actor state for replication, policy, and persistence readers. */
struct ActorSnapshot final {
    ActorKey key{};
    PolicyOwnerId policyOwnerId{};
    BlueprintRef blueprint{};
    ActorKey parent{};
    Transform transform{};
    MotionAuthority authority{};
    std::uint64_t revision{};
    std::uint32_t bubble{};
    ActorLifecycle lifecycle{ActorLifecycle::live};
};

/** One host-service pose proposed for an atomic canonical-state commit. */
struct CanonicalPoseCommit final {
    ActorKey actor{};
    Transform transform{};
    CommandSource source{};
    PolicyOwnerId policyOwnerId{};
    std::uint64_t authorityGeneration{};
};

/** Pointer-free event kinds published after world mutation. */
enum class CommittedEventKind : std::uint8_t {
    actorSpawned,
    actorRemoved,
    authorityChanged,
    transformChanged,
    kinematicTargetChanged,
    actorTeleported,
    timerFired,
    checkpointRequested,
    rewardIntentSubmitted,
    controllerConfigured,
    pathRequested,
    combatConfigured,
    damageCommitted,
    triggerCreated,
    triggerRemoved,
    objectiveChanged,
    participantCreditChanged,
    incidentSubmitted,
    serviceTickCommitted,
    count,
};

/** A tick publishes this value only after its world mutation commits. */
struct CommittedEvent final {
    ActorKey actor{};
    Transform transform{};
    CommandId commandId{};
    TickId tick{};
    std::uint64_t actorRevision{};
    std::uint64_t authorityGeneration{};
    std::uint64_t value{};
    CommittedEventKind kind{CommittedEventKind::actorSpawned};
};

/** Bounded reasons for commands rejected by the writer. */
enum class CommandRejectReason : std::uint8_t {
    none,
    invalid,
    wrongWorld,
    wrongOwner,
    staleActor,
    staleAuthority,
    duplicate,
    capacity,
    policyDenied,
    late,
    count,
};

/** Metrics allocate one counter for every closed command-rejection reason. */
inline constexpr std::size_t kCommandRejectReasonCount =
    static_cast<std::size_t>(CommandRejectReason::count);

/** Bounded counters copied without log or transport state. */
struct WorldMetrics final {
    std::array<std::uint64_t, kCommandRejectReasonCount> rejectedByReason{};
    std::uint64_t commandsApplied{};
    std::uint64_t commandsRejected{};
    std::uint64_t ticksRun{};
    std::uint64_t queueOverflows{};
};

/** Fixed immutable copy of one committed world tick. */
struct WorldSnapshot final {
    std::array<ActorSnapshot, kActorCapacity> actors{};
    std::array<RetiredActorGeneration, kActorGenerationHistoryCapacity> retiredActors{};
    std::array<HostCommandHeader, kCommandHistoryCapacity> commandHistory{};
    WorldIdentity identity{};
    TickId tick{};
    std::uint64_t canonicalHash{};
    std::size_t actorCount{};
    std::size_t retiredActorCount{};
    std::size_t commandHistoryCount{};
    std::size_t commandHistoryCursor{};
    bool healthy{};
};

/** @return True when every world identity generation matches. */
[[nodiscard]] bool equal_world(const WorldIdentity& left, const WorldIdentity& right) noexcept;
/** @return True when logical id and actor generation match. */
[[nodiscard]] bool equal_actor(const ActorKey& left, const ActorKey& right) noexcept;
/** @return True when no world identity field uses its zero sentinel. */
[[nodiscard]] bool valid_world(const WorldIdentity& identity) noexcept;
/** @return True when actor id and generation are nonzero. */
[[nodiscard]] bool valid_actor(const ActorKey& actor) noexcept;
/** @return True when definition id and version are nonzero. */
[[nodiscard]] bool valid_blueprint(const BlueprintRef& blueprint) noexcept;
/** @return True when translation and a nonzero rotation are finite. */
[[nodiscard]] bool finite_transform(const Transform& transform) noexcept;
/** @return True after finite transforms use one normalized bit representation. */
[[nodiscard]] bool canonicalize_transform(Transform& transform) noexcept;
/** @return Canonical hash of one validated pointer-free world snapshot. */
[[nodiscard]] std::uint64_t compute_snapshot_hash(const WorldSnapshot& snapshot) noexcept;

} // namespace sunrise::server::gameplay::physics::world
