#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../world/world_types.h"

namespace sunrise::server::gameplay::physics::authority {

/** Motion validation tracks at most the logical actor capacity for one world. */
inline constexpr std::size_t kMotionActorCapacity = world::kActorCapacity;

/** Exact protected peer/view lifetime allowed to propose motion. */
struct MotionPeerKey final {
    std::uint64_t memberId{};
    std::uint64_t associationGeneration{};
    std::uint64_t channelGeneration{};
    std::uint64_t viewGeneration{};
};

/** Versioned bounds supplied by content or host policy. */
struct MotionLimits final {
    world::BlueprintRef profile{};
    std::uint64_t maximumTickLag{};
    float maximumDisplacementPerTick{};
    float maximumSpeed{};
    float maximumAccelerationPerTick{};
    float quaternionTolerance{};
    bool requireCollisionFree{};
    bool requireGrounded{};
};

/** Authoritative actor state used to configure one validator record. */
struct MotionBaseline final {
    world::WorldIdentity world{};
    world::ActorKey actor{};
    world::Transform transform{};
    world::Vector3 linearVelocity{};
    world::MotionAuthority authority{};
    MotionPeerKey peer{};
    MotionLimits limits{};
    world::TickId tick{};
    std::uint32_t bubble{};
};

/** One client-predicted candidate after protected transport authentication. */
struct MotionCandidate final {
    world::WorldIdentity world{};
    world::ActorKey actor{};
    world::Transform transform{};
    world::Vector3 linearVelocity{};
    MotionPeerKey peer{};
    std::uint64_t authorityGeneration{};
    std::uint64_t packetSequence{};
    world::TickId candidateTick{};
    world::TickId receiptTick{};
    std::uint32_t bubble{};
};

/** Host collision evidence for one already bounded candidate. */
struct MotionEvidence final {
    bool collisionFree{};
    bool grounded{};
};

enum class MotionDecision : std::uint8_t {
    accepted,
    correction,
    invalid,
    notConfigured,
    wrongContext,
    wrongOwner,
    stale,
    duplicate,
    limitExceeded,
    collisionRejected,
};

/** Decision and authoritative state returned without exposing storage. */
struct MotionResult final {
    MotionBaseline authoritative{};
    MotionDecision decision{MotionDecision::invalid};
};

/** Fixed client-prediction validator with a host-owned default. */
class MotionValidator final {
public:
    void reset() noexcept;
    [[nodiscard]] bool configure(const MotionBaseline& baseline) noexcept;
    [[nodiscard]] bool remove(const world::ActorKey& actor) noexcept;
    [[nodiscard]] MotionDecision evaluate(const MotionCandidate& candidate,
                                          const MotionEvidence& evidence,
                                          MotionResult& result) noexcept;
    [[nodiscard]] bool query(const world::ActorKey& actor, MotionBaseline& baseline) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Record final {
        MotionBaseline baseline{};
        std::uint64_t lastPacketSequence{};
        bool occupied{};
    };

    [[nodiscard]] Record* find(const world::ActorKey& actor) noexcept;
    [[nodiscard]] const Record* find(const world::ActorKey& actor) const noexcept;

    std::array<Record, kMotionActorCapacity> records_{};
    std::size_t size_{};
};

} // namespace sunrise::server::gameplay::physics::authority
