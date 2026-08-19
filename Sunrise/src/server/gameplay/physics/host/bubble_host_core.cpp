#include <new>

#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {

/** Allocates the fixed host registry and large journal without throwing. */
BubbleHost::BubbleHost() noexcept : storage_(new(std::nothrow) Storage{}) {
    if (storage_ != nullptr) {
        for (std::unique_ptr<Storage::WorldSlot>& slot : storage_->worlds.slots) {
            slot.reset(new (std::nothrow) Storage::WorldSlot{});
            if (slot == nullptr) {
                return;
            }
        }
        storage_->journal.reset(new (std::nothrow) persistence::CommandJournal{});
    }
}

/** Releases peer planners before their world-owned services disappear. */
BubbleHost::~BubbleHost() {
    if (storage_ == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < storage_->worlds.size(); ++index) {
        Storage::WorldSlot& slot = storage_->worlds[index];
        if (!slot.active) {
            continue;
        }
        static_cast<void>(detail::reset_peer_services(slot));
        slot.runner.close();
        slot.builtInController.shutdown();
        slot.navigation.shutdown();
        slot.physics.shutdown();
    }
}

/** Reports whether the fixed registry allocation succeeded. */
bool BubbleHost::ready() const noexcept {
    if (storage_ == nullptr || storage_->journal == nullptr) {
        return false;
    }
    for (const std::unique_ptr<Storage::WorldSlot>& slot : storage_->worlds.slots) {
        if (slot == nullptr) {
            return false;
        }
    }
    return true;
}

/** Counts active exact world lifetimes. */
std::size_t BubbleHost::world_count() const noexcept {
    return storage_ == nullptr ? 0 : storage_->worldCount;
}

/** Resolves one exact world handle without permitting generation aliases. */
std::size_t BubbleHost::find_slot(WorldHandle handle) const noexcept {
    if (storage_ == nullptr || !detail::valid_handle_shape(handle)) {
        return kWorldCapacity;
    }
    const Storage::WorldSlot& slot = storage_->worlds[handle.slot];
    return slot.active && slot.handleGeneration == handle.generation ? handle.slot : kWorldCapacity;
}

/** Resolves one exact runner identity for executor callbacks. */
std::size_t BubbleHost::find_world(const world::WorldIdentity& identity) const noexcept {
    if (storage_ == nullptr || !world::valid_world(identity)) {
        return kWorldCapacity;
    }
    for (std::size_t index = 0; index < storage_->worlds.size(); ++index) {
        const Storage::WorldSlot& slot = storage_->worlds[index];
        if (slot.active && world::equal_world(slot.runner.identity(), identity)) {
            return index;
        }
    }
    return kWorldCapacity;
}

/** Records one bounded stage for deterministic tick diagnostics. */
void BubbleHost::record_stage(std::size_t slotIndex, TickStage stage) noexcept {
    if (storage_ == nullptr || slotIndex >= storage_->worlds.size()) {
        return;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (slot.tickStageCount < slot.tickStages.size()) {
        slot.tickStages[slot.tickStageCount++] = stage;
    } else {
        slot.healthy = false;
    }
}

} // namespace sunrise::server::gameplay::physics::host
