#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../interest/interest_manager.h"
#include "external_plan.h"
#include "world_projection.h"

namespace sunrise::server::gameplay::physics::replication {

/** Live actors and draining generations may coexist during peer removal retries. */
inline constexpr std::size_t kCoordinatorActorCapacity = world::kActorCapacity * 2U;
/** The State activity context fixes the number of protected peer replicas. */
inline constexpr std::size_t kCoordinatorPeerCapacity =
    state::gameplay::physics::kPeerReplicaCapacity;

/** Optional generic projection inputs. An empty span selects the scriptless defaults. */
struct ActorProjectionOptions final {
    world::ActorKey actor{};
    std::uint64_t combatRevision{};
    bool alwaysRelevant{};
};

/** Result of one canonical world reconciliation. */
enum class WorldReconcileResult : std::uint8_t {
    applied,
    unchanged,
    invalid,
    stale,
    capacity,
    invariantFailure,
};

/** Result of applying one relevance pass to an exact peer planner. */
enum class PeerSyncResult : std::uint8_t {
    applied,
    unchanged,
    invalid,
    stale,
    capacity,
    invariantFailure,
};

/** Result of preparing one immutable frame and its pending outcome owners. */
enum class FramePrepareResult : std::uint8_t {
    ready,
    idle,
    full,
    invalid,
    stale,
    invariantFailure,
};

/** Common and entity owners attached to one exact packet generation. */
struct FrameContribution final {
    middleware::gameplay::external::ExternalEntityFrame frame{};
    common::CommonContributionHandle common{};
    ContributionHandle entity{};
    std::uint64_t coordinatorGeneration{};
    std::uint64_t peerGeneration{};
    std::uint64_t packetGeneration{};
    bool commonPresent{};
    bool entityPresent{};
};

/** Fixed world-to-replication owner. The caller serializes every mutation. */
class WorldCoordinator final {
public:
    WorldCoordinator() = default;
    WorldCoordinator(const WorldCoordinator&) = delete;
    WorldCoordinator& operator=(const WorldCoordinator&) = delete;

    /**
     * Binds one exact world to one published activity allocator.
     * @param identity Exact world and owner lifetime.
     * @param context Exact State activity context.
     * @return True when an empty coordinator accepted the binding.
     */
    [[nodiscard]] bool bind(const world::WorldIdentity& identity,
                            state::gameplay::physics::ContextHandle context) noexcept;

    /**
     * Reconciles one committed world snapshot into stable allocation identities.
     * @param snapshot Canonical live world actors.
     * @param options Optional exact per-actor combat and relevance inputs.
     * @return Applied, unchanged, or a fail-closed reason.
     */
    [[nodiscard]] WorldReconcileResult
    reconcile(const world::WorldSnapshot& snapshot,
              std::span<const ActorProjectionOptions> options = {}) noexcept;

    /**
     * Registers the exact host-owned planner object used for later calls.
     * @param peer Initialized empty planner with an exact State replica.
     * @param key Host-owned interest baseline identity.
     * @return True when fixed registry capacity accepted the peer.
     */
    [[nodiscard]] bool register_peer(PeerState& peer, const interest::PeerKey& key) noexcept;

    /**
     * Releases planner-owned references before its host peer lifetime ends.
     * @param peer Exact object passed to register_peer.
     * @return True when the planner and registry entry drained.
     */
    [[nodiscard]] bool unregister_peer(PeerState& peer) noexcept;

    /**
     * Applies one host-owned interest pass in dependency-safe order.
     * @param manager Interest manager that already owns the registered key.
     * @param peer Exact registered planner object.
     * @param view Current spatial view for that peer lifetime.
     * @param output Receives transitions only after every planner mutation succeeds.
     * @return Applied, unchanged, or a fail-closed reason.
     */
    [[nodiscard]] PeerSyncResult synchronize_peer(interest::InterestManager& manager,
                                                  PeerState& peer,
                                                  const interest::PeerView& view,
                                                  interest::Evaluation& output) noexcept;

    /**
     * Builds one frame and attaches its common/entity owners to a packet.
     * @param peer Exact registered planner object.
     * @param commonState Exact common state for the same ActivityClient lifetime.
     * @param packetGeneration Caller-reserved nonzero packet generation.
     * @param payload Optional type state. Null selects the empty type-0 fallback.
     * @param output Receives the immutable frame and exact pending handles on success.
     * @return Ready, idle, or a fail-closed reason.
     */
    [[nodiscard]] FramePrepareResult prepare_frame(PeerState& peer,
                                                   common::State& commonState,
                                                   std::uint64_t packetGeneration,
                                                   const EntityPayloadPlan* payload,
                                                   FrameContribution& output) noexcept;

    /**
     * Applies one exact external contribution outcome once.
     * @param peer Exact registered planner object.
     * @param commonState Exact common state used by prepare_frame.
     * @param contribution Immutable pending contribution.
     * @param packetGeneration Exact packet generation reported by transport.
     * @param outcome Eligible external contribution result.
     * @return True when every present owner settled once.
     */
    [[nodiscard]] bool settle_frame(PeerState& peer,
                                    common::State& commonState,
                                    const FrameContribution& contribution,
                                    std::uint64_t packetGeneration,
                                    Outcome outcome) noexcept;

    /** @return Number of drained allocation generations retired in this call. */
    [[nodiscard]] std::size_t retire_drained() noexcept;

    /**
     * Copies one live or draining allocation without changing output on failure.
     * @param actor Exact logical actor generation.
     * @param output Receives its stable allocation identity.
     * @return True when the generation remains tracked.
     */
    [[nodiscard]] bool allocation_for(const world::ActorKey& actor,
                                      state::gameplay::physics::Allocation& output) const noexcept;

    /** @return True while one exact world and activity context are bound. */
    [[nodiscard]] bool bound() const noexcept;
    /** @return Number of live and draining actor generations. */
    [[nodiscard]] std::size_t actor_count() const noexcept;
    /** @return Number of exact registered planner objects. */
    [[nodiscard]] std::size_t peer_count() const noexcept;
    /** @return True after an unexpected cross-service commit failure. */
    [[nodiscard]] bool healthy() const noexcept;

private:
    struct ActorRecord final {
        WorldProjectionState projection{};
        ActorSnapshot projected{};
        state::gameplay::physics::Allocation allocation{};
        world::ActorKey key{};
        bool alwaysRelevant{};
        bool occupied{};
        bool live{};
        bool removeQueued{};
    };

    struct PeerRecord final {
        PeerState* planner{};
        interest::PeerKey interest{};
        state::gameplay::physics::PeerReplicaHandle replica{};
        bool occupied{};
    };

    [[nodiscard]] std::size_t find_actor(const world::ActorKey& actor) const noexcept;
    [[nodiscard]] std::size_t find_actor_id(world::ActorId actorId) const noexcept;
    [[nodiscard]] std::size_t find_peer(const PeerState& peer) const noexcept;
    [[nodiscard]] bool context_is_current() const noexcept;
    [[nodiscard]] bool
    validate_state_rows(std::span<const world::ActorKey> newActors) const noexcept;

    std::array<ActorRecord, kCoordinatorActorCapacity> actors_{};
    std::array<PeerRecord, kCoordinatorPeerCapacity> peers_{};
    std::array<ActorRecord, kCoordinatorActorCapacity> actorScratch_{};
    std::array<bool, kCoordinatorActorCapacity> seenScratch_{};
    std::array<world::ActorKey, world::kActorCapacity> newActorScratch_{};
    std::array<std::size_t, world::kActorCapacity> newSlotScratch_{};
    std::array<state::gameplay::physics::Allocation, world::kActorCapacity> allocationScratch_{};
    std::array<interest::ActorInput, world::kActorCapacity> interestInputScratch_{};
    interest::InterestManager interestScratch_{};
    interest::Evaluation evaluationScratch_{};
    PeerState peerScratch_{};
    state::gameplay::physics::ActivityReplicaContext activity_{};
    state::gameplay::physics::ContextHandle context_{};
    world::WorldIdentity world_{};
    world::TickId lastTick_{};
    std::uint64_t lastHash_{};
    std::uint64_t generation_{};
    std::size_t actorCount_{};
    std::size_t peerCount_{};
    bool bound_{};
    bool hasSnapshot_{};
    bool healthy_{true};
};

/** @return Empty type-0 callbacks for scriptless frame encode and decode. */
[[nodiscard]] const middleware::gameplay::external::TypePayloadCodec&
scriptless_payload_codec() noexcept;

} // namespace sunrise::server::gameplay::physics::replication
