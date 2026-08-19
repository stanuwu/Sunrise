#include <algorithm>

#include "actor_store.h"

namespace sunrise::server::gameplay::physics::world {

/**
 * Finds the latest retired generation for one logical actor id.
 * @param actorId Stable logical actor id.
 * @return Latest retired generation, or zero when unseen.
 */
std::uint64_t ActorStore::last_generation(ActorId actorId) const noexcept {
    std::uint64_t generation = 0;
    for (std::size_t index = 0; index < tombstoneCount_; ++index) {
        const RetiredActorGeneration& tombstone = tombstones_[index];
        if (tombstone.actorId == actorId && tombstone.generation > generation) {
            generation = tombstone.generation;
        }
    }
    return generation;
}

/**
 * Checks exact retired-generation capacity before actor removal.
 * @param actorId Stable logical actor id.
 * @return True when an existing or new fixed slot can retain it.
 */
bool ActorStore::can_remember_generation(ActorId actorId) const noexcept {
    if (tombstoneCount_ < tombstones_.size()) {
        return true;
    }
    for (const RetiredActorGeneration& tombstone : tombstones_) {
        if (tombstone.actorId == actorId) {
            return true;
        }
    }
    return false;
}

/** Retains one actor high-water generation at its retirement tick. */
void ActorStore::remember_generation(ActorKey actor, TickId retiredTick) noexcept {
    for (std::size_t index = 0; index < tombstoneCount_; ++index) {
        RetiredActorGeneration& tombstone = tombstones_[index];
        if (tombstone.actorId == actor.id) {
            tombstone.generation = std::max(tombstone.generation, actor.generation);
            tombstone.retiredTick = std::max(tombstone.retiredTick, retiredTick);
            return;
        }
    }
    if (tombstoneCount_ < tombstones_.size()) {
        tombstones_[tombstoneCount_++] = {actor.id, actor.generation, retiredTick};
    }
}

/** Copies retired actor high-water entries in canonical actor-id order. */
std::size_t ActorStore::copy_retired(std::span<RetiredActorGeneration> output) const noexcept {
    const std::size_t count = std::min(tombstoneCount_, output.size());
    std::copy_n(tombstones_.begin(), count, output.begin());
    std::sort(
        output.begin(),
        output.begin() + static_cast<std::ptrdiff_t>(count),
        [](const RetiredActorGeneration& first, const RetiredActorGeneration& second) noexcept {
            return first.actorId < second.actorId;
        });
    return count;
}

/** Releases actor high-water entries below one durable replay floor. */
void ActorStore::expire_generations_before(TickId firstRetainedTick) noexcept {
    std::size_t retained = 0;
    for (std::size_t index = 0; index < tombstoneCount_; ++index) {
        if (tombstones_[index].retiredTick >= firstRetainedTick) {
            tombstones_[retained++] = tombstones_[index];
        }
    }
    for (std::size_t index = retained; index < tombstoneCount_; ++index) {
        tombstones_[index] = {};
    }
    tombstoneCount_ = retained;
}

} // namespace sunrise::server::gameplay::physics::world
