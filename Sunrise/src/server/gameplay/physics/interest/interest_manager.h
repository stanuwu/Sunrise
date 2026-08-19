#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../world/world_types.h"

namespace sunrise::server::gameplay::physics::interest {

/** One world supports 32 peers and at most one enter plus one leave per actor. */
inline constexpr std::size_t kPeerCapacity = 32;
inline constexpr std::size_t kTransitionCapacity = world::kActorCapacity * 2U;

/** Exact peer and view lifetime that owns one relevance baseline. */
struct PeerKey final {
    std::uint64_t memberId{};
    std::uint64_t viewGeneration{};
    std::uint64_t activitySessionId{};
};

/** Current peer position and bounded distance thresholds. */
struct PeerView final {
    PeerKey key{};
    world::Vector3 position{};
    float enterDistance{};
    float leaveDistance{};
    std::uint32_t bubble{};
};

/** Pointer-free actor input used by the relevance pass. */
struct ActorInput final {
    world::ActorKey key{};
    world::ActorKey parent{};
    world::ActorKey streamSource{};
    world::Vector3 position{};
    std::uint32_t bubble{};
    bool alwaysRelevant{};
};

enum class TransitionKind : std::uint8_t {
    entered,
    stayed,
    left,
};

/** One deterministic relevance change for a peer. */
struct Transition final {
    world::ActorKey actor{};
    TransitionKind kind{TransitionKind::entered};
};

/** Bounded result of one committed relevance pass. */
struct Evaluation final {
    std::array<Transition, kTransitionCapacity> transitions{};
    std::size_t count{};
};

/** Fixed-storage relevance manager with per-view hysteresis. */
class InterestManager final {
public:
    void reset() noexcept;
    [[nodiscard]] bool bind_peer(const PeerKey& key) noexcept;
    [[nodiscard]] bool unbind_peer(const PeerKey& key) noexcept;
    [[nodiscard]] bool evaluate(const world::WorldIdentity& identity,
                                const PeerView& view,
                                std::span<const ActorInput> actors,
                                Evaluation& output) noexcept;
    [[nodiscard]] std::size_t peer_count() const noexcept;

private:
    struct RelevantActor final {
        world::ActorKey key{};
        world::ActorKey parent{};
        world::ActorKey streamSource{};
        bool occupied{};
    };

    struct PeerRecord final {
        std::array<RelevantActor, world::kActorCapacity> actors{};
        PeerKey key{};
        world::WorldIdentity world{};
        bool occupied{};
        bool hasWorld{};
    };

    [[nodiscard]] PeerRecord* find_peer(const PeerKey& key) noexcept;
    [[nodiscard]] const PeerRecord* find_peer(const PeerKey& key) const noexcept;
    [[nodiscard]] static bool was_relevant(const PeerRecord& peer,
                                           const world::ActorKey& key) noexcept;
    [[nodiscard]] static bool order_transitions(const PeerRecord& peer,
                                                std::span<const ActorInput> actors,
                                                std::span<const world::ActorKey> next,
                                                Evaluation& output) noexcept;

    std::array<PeerRecord, kPeerCapacity> peers_{};
    std::size_t peerCount_{};
};

} // namespace sunrise::server::gameplay::physics::interest
