#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "host_command.h"

namespace sunrise::server::gameplay::physics::world {

/** Exact result of one logical actor mutation. */
enum class ActorStoreStatus : std::uint8_t {
    applied,
    invalid,
    notFound,
    staleActor,
    duplicate,
    capacity,
    generationHistoryFull,
};

/** Fixed logical actor state owned only by the world runner. */
class ActorStore final {
public:
    /** Clears live actors and retired actor-generation guards. */
    void reset() noexcept;
    /** @return Applied or one exact spawn rejection. */
    [[nodiscard]] ActorStoreStatus spawn(const HostCommandHeader& header,
                                         const SpawnActorCommand& command,
                                         TickId tick,
                                         ActorSnapshot& committed) noexcept;
    /** @return Applied only when the exact child-free actor exists. */
    [[nodiscard]] ActorStoreStatus
    remove(const ActorKey& actor, TickId retiredTick, ActorSnapshot& removed) noexcept;
    /** @return Applied only when actor identity and transform are valid. */
    [[nodiscard]] ActorStoreStatus set_transform(const ActorKey& actor,
                                                 const Transform& transform,
                                                 ActorSnapshot& committed) noexcept;
    /** @return Applied only after authority transition validation. */
    [[nodiscard]] ActorStoreStatus set_authority(const ActorKey& actor,
                                                 const MotionAuthority& authority,
                                                 ActorSnapshot& committed) noexcept;
    /** @return True when the exact live actor was copied. */
    [[nodiscard]] bool find(const ActorKey& actor, ActorSnapshot& snapshot) const noexcept;
    /** @return True when any generation of the actor id is live. */
    [[nodiscard]] bool find_by_id(ActorId actorId, ActorSnapshot& snapshot) const noexcept;
    /** @return Number of live actors copied in logical identity order. */
    [[nodiscard]] std::size_t copy_sorted(std::span<ActorSnapshot> output) const noexcept;
    /** @return Number of live actors. */
    [[nodiscard]] std::size_t size() const noexcept;
    /** @return Number of retired generation guards copied in actor-id order. */
    [[nodiscard]] std::size_t copy_retired(std::span<RetiredActorGeneration> output) const noexcept;
    /** @return True when validated live and retired snapshots replaced the store. */
    [[nodiscard]] bool restore(std::span<const ActorSnapshot> actors,
                               std::span<const RetiredActorGeneration> retired) noexcept;
    /** @return True when live and retired snapshots form canonical state. */
    [[nodiscard]] static bool
    valid_restore(std::span<const ActorSnapshot> actors,
                  std::span<const RetiredActorGeneration> retired) noexcept;

private:
    friend class WorldRunner;

    struct Slot final {
        ActorSnapshot actor{};
        bool occupied{};
    };

    [[nodiscard]] Slot* find_slot(const ActorKey& actor) noexcept;
    [[nodiscard]] const Slot* find_slot(const ActorKey& actor) const noexcept;
    [[nodiscard]] Slot* find_id_slot(ActorId actorId) noexcept;
    [[nodiscard]] const Slot* find_id_slot(ActorId actorId) const noexcept;
    [[nodiscard]] std::uint64_t last_generation(ActorId actorId) const noexcept;
    [[nodiscard]] bool can_remember_generation(ActorId actorId) const noexcept;
    void remember_generation(ActorKey actor, TickId retiredTick) noexcept;
    void expire_generations_before(TickId firstRetainedTick) noexcept;
    [[nodiscard]] bool commit_pose_batch(std::span<const CanonicalPoseCommit> commits,
                                         std::span<ActorSnapshot> committed) noexcept;

    std::array<Slot, kActorCapacity> slots_{};
    std::array<RetiredActorGeneration, kActorGenerationHistoryCapacity> tombstones_{};
    std::size_t size_{};
    std::size_t tombstoneCount_{};
};

} // namespace sunrise::server::gameplay::physics::world
