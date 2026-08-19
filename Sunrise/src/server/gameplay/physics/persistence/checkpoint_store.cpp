#include <algorithm>
#include <limits>

#include "checkpoint.h"

namespace sunrise::server::gameplay::physics::persistence {
namespace {

[[nodiscard]] bool same_checkpoint(const Checkpoint& left, const Checkpoint& right) noexcept {
    return left.checkpointId == right.checkpointId && left.canonicalHash == right.canonicalHash
           && left.journalCursor == right.journalCursor
           && left.world.canonicalHash == right.world.canonicalHash;
}

} // namespace

/** Clears stored checkpoints while preserving exhausted generation markers. */
void CheckpointStore::reset() noexcept {
    for (Slot& slot : slots_) {
        const std::uint64_t generation = slot.generation;
        slot = {};
        slot.generation = generation;
    }
    size_ = 0;
}

/** Saves or replaces one logical world's newest checkpoint. */
StoreResult CheckpointStore::save(const Checkpoint& checkpoint, CheckpointHandle& handle) noexcept {
    handle = {};
    if (!valid_checkpoint(checkpoint)) {
        return StoreResult::invalid;
    }
    std::size_t target = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        Slot& slot = slots_[index];
        if (slot.occupied && slot.checkpoint.logicalWorldId == checkpoint.logicalWorldId) {
            if (checkpoint.checkpointId < slot.checkpoint.checkpointId) {
                return StoreResult::stale;
            }
            if (checkpoint.checkpointId == slot.checkpoint.checkpointId) {
                if (!same_checkpoint(slot.checkpoint, checkpoint)) {
                    return StoreResult::invalid;
                }
                handle = {slot.generation, index};
                return StoreResult::duplicate;
            }
            target = index;
            break;
        }
        if (!slot.occupied && slot.generation != (std::numeric_limits<std::uint64_t>::max)()
            && target == slots_.size()) {
            target = index;
        }
    }
    if (target == slots_.size()) {
        return size_ == slots_.size() ? StoreResult::capacity : StoreResult::exhausted;
    }
    Slot& slot = slots_[target];
    if (slot.generation == (std::numeric_limits<std::uint64_t>::max)()) {
        return StoreResult::exhausted;
    }
    const bool wasOccupied = slot.occupied;
    const std::uint64_t generation = slot.generation + 1U;
    slot.checkpoint = checkpoint;
    slot.generation = generation;
    slot.occupied = true;
    size_ += wasOccupied ? 0U : 1U;
    handle = {generation, target};
    return StoreResult::saved;
}

/** Copies one exact stored checkpoint. */
bool CheckpointStore::load(CheckpointHandle handle, Checkpoint& checkpoint) const noexcept {
    checkpoint = {};
    if (handle.slot >= slots_.size() || handle.generation == 0) {
        return false;
    }
    const Slot& slot = slots_[handle.slot];
    if (!slot.occupied || slot.generation != handle.generation) {
        return false;
    }
    checkpoint = slot.checkpoint;
    return true;
}

/** Finds the retained checkpoint for one logical world. */
bool CheckpointStore::latest(std::uint64_t logicalWorldId,
                             CheckpointHandle& handle) const noexcept {
    handle = {};
    if (logicalWorldId == 0) {
        return false;
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].occupied && slots_[index].checkpoint.logicalWorldId == logicalWorldId) {
            handle = {slots_[index].generation, index};
            return true;
        }
    }
    return false;
}

/** Erases one exact retained checkpoint and invalidates its handle. */
StoreResult CheckpointStore::erase(CheckpointHandle handle) noexcept {
    if (handle.slot >= slots_.size() || handle.generation == 0) {
        return StoreResult::notFound;
    }
    Slot& slot = slots_[handle.slot];
    if (!slot.occupied || slot.generation != handle.generation) {
        return StoreResult::notFound;
    }
    slot.checkpoint = {};
    slot.occupied = false;
    --size_;
    return StoreResult::erased;
}

/** Returns the number of retained logical worlds. */
std::size_t CheckpointStore::size() const noexcept {
    return size_;
}

} // namespace sunrise::server::gameplay::physics::persistence
