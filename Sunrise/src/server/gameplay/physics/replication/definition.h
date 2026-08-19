#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../../state/gameplay/physics/runtime.h"

namespace sunrise::server::gameplay::physics::replication {

/** One peer tracks every actor allowed by the first external envelope. */
inline constexpr std::size_t kActorCapacity =
    state::gameplay::physics::kPeerAllocationReferenceCapacity;
/** Outcome storage matches the exact ticket capacity owned by State. */
inline constexpr std::size_t kInFlightCapacity =
    state::gameplay::physics::kContributionTicketCapacity;
/** A zero generation never names a live planner record. */
inline constexpr std::uint64_t kAbsentGeneration = 0;

/** Replicated fields ordered by lifecycle importance. */
enum class Field : std::uint8_t {
    lifecycle,
    authority,
    parent,
    combat,
    content,
    transform,
    count,
};

/** Fixed field bits carried by one update plan. */
using FieldMask = std::uint32_t;

/** One bit for a replicated field. */
[[nodiscard]] constexpr FieldMask field_bit(Field field) noexcept {
    return FieldMask{1U} << static_cast<std::uint8_t>(field);
}

/** All fields needed by one create baseline. */
inline constexpr FieldMask kCreateFieldMask =
    field_bit(Field::lifecycle) | field_bit(Field::authority) | field_bit(Field::parent)
    | field_bit(Field::combat) | field_bit(Field::content) | field_bit(Field::transform);

/** Canonical transform copied into an immutable replication plan. */
struct Transform final {
    std::array<float, 4> orientation{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 3> position{};
};

/** Monotonic versions used to compare current state with one peer baseline. */
struct FieldVersions final {
    std::uint64_t lifecycle{};
    std::uint64_t authority{};
    std::uint64_t parent{};
    std::uint64_t combat{};
    std::uint64_t content{};
    std::uint64_t transform{};
};

/** Immutable actor data accepted by the generic planner. */
struct ActorSnapshot final {
    state::gameplay::physics::Allocation allocation{};
    FieldVersions versions{};
    Transform transform{};
    std::uint64_t parentActorId{};
    std::uint64_t streamSourceActorId{};
    std::uint64_t authorityEpoch{};
    std::uint64_t combatRevision{};
    std::uint64_t contentRevision{};
    std::uint32_t bubble{};
};

/** Lifecycle operation carried by one channel-two contribution. */
enum class Operation : std::uint8_t {
    none,
    create,
    update,
    remove,
};

/** Result of one eligible external contribution. */
enum class Outcome : std::uint8_t {
    success,
    lost,
};

/** Immutable one-actor contribution prepared for a packet transaction. */
struct Plan final {
    ActorSnapshot actor{};
    std::uint64_t peerGeneration{};
    std::uint64_t recordGeneration{};
    FieldMask fields{};
    Operation operation{Operation::none};
    std::size_t actorSlot{kActorCapacity};
};

/** Exact handle to one pending external outcome. */
struct ContributionHandle final {
    std::uint64_t peerGeneration{};
    std::uint64_t generation{};
    std::uint64_t packetGeneration{};
    std::size_t slot{kInFlightCapacity};
};

/** Planner result when no packet contribution is required. */
enum class PrepareResult : std::uint8_t {
    ready,
    idle,
    full,
    invalid,
};

/** One actor's desired state and committed baseline for one peer. */
struct ActorRecord final {
    ActorSnapshot desired{};
    FieldVersions baseline{};
    std::uint64_t baselineParentActorId{};
    std::uint64_t baselineStreamSourceActorId{};
    std::uint64_t generation{};
    bool createCommitted{};
    bool removeRequested{};
    bool contributionPending{};
    bool occupied{};
};

/** Exact versions retained until one eligible outcome settles. */
struct InFlightRecord final {
    state::gameplay::physics::ContributionTicket ticket{};
    ActorSnapshot actor{};
    std::uint64_t generation{};
    std::uint64_t packetGeneration{};
    std::uint64_t actorRecordGeneration{};
    FieldMask fields{};
    Operation operation{Operation::none};
    std::size_t actorSlot{kActorCapacity};
    bool occupied{};
};

/** Per-peer planner state. The world writer owns every mutation. */
struct PeerState final {
    state::gameplay::physics::PeerReplicaHandle replica{};
    std::array<ActorRecord, kActorCapacity> actors{};
    std::array<InFlightRecord, kInFlightCapacity> inFlight{};
    std::uint64_t generation{};
    std::uint64_t nextRecordGeneration{1};
    std::uint64_t nextContributionGeneration{1};
    bool occupied{};
};

} // namespace sunrise::server::gameplay::physics::replication
