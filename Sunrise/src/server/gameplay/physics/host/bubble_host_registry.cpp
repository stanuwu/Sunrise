#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {

/** Submits one exact command only while both host and runner are healthy. */
CommandSubmitResult BubbleHost::submit(WorldHandle handle,
                                       const world::HostCommand& command) noexcept {
    CommandSubmitResult result{};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        result.status = storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
        return result;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (!slot.healthy || !slot.runner.healthy()) {
        result.status = HostStatus::unhealthy;
        return result;
    }
    result.command = slot.runner.submit(command);
    result.status = HostStatus::success;
    return result;
}

/** Copies one immutable logical world snapshot. */
HostStatus BubbleHost::snapshot(WorldHandle handle, world::WorldSnapshot& output) const noexcept {
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        output = {};
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    return storage_->worlds[slotIndex].runner.snapshot(output) ? HostStatus::success
                                                               : HostStatus::unhealthy;
}

/** Copies bounded writer metrics without exposing runner mutation. */
HostStatus BubbleHost::world_metrics(WorldHandle handle,
                                     world::WorldMetrics& output) const noexcept {
    output = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    output = storage_->worlds[slotIndex].runner.metrics();
    return HostStatus::success;
}

/** Copies exact scene handles and fixed backend capabilities. */
HostStatus BubbleHost::resources(WorldHandle handle, WorldResources& output) const noexcept {
    output = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    output.identity = slot.runner.identity();
    output.physicsScene = slot.physicsScene;
    output.navigationScene = slot.navigationScene;
    output.physics = slot.physics.capabilities();
    output.navigation = slot.navigation.capabilities();
    output.logicalWorldId = slot.logicalWorldId;
    output.journalCursor = slot.journalCursor;
    return HostStatus::success;
}

} // namespace sunrise::server::gameplay::physics::host
