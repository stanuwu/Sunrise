#include <algorithm>

#include "bubble_host.h"
#include "internal.h"

namespace sunrise::server::gameplay::physics::host {

/** Moves fallback checkpoint requests without partial output. */
HostStatus BubbleHost::drain_checkpoint_requests(WorldHandle handle,
                                                 std::span<CheckpointRequest> requests,
                                                 std::size_t& requestCount) noexcept {
    requestCount = 0;
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (requests.size() < slot.checkpointRequestCount) {
        return HostStatus::capacity;
    }
    std::copy_n(slot.checkpointRequests.begin(), slot.checkpointRequestCount, requests.begin());
    requestCount = slot.checkpointRequestCount;
    slot.checkpointRequests = {};
    slot.checkpointRequestCount = 0;
    return HostStatus::success;
}

/** Copies contacts drained from the last fixed physics step. */
HostStatus BubbleHost::copy_last_contacts(WorldHandle handle,
                                          backend::ContactBuffer& contacts) const noexcept {
    contacts = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    contacts = storage_->worlds[slotIndex].contacts;
    return HostStatus::success;
}

/** Copies trigger edges from the last fixed service step. */
HostStatus BubbleHost::copy_last_trigger_events(WorldHandle handle,
                                                std::span<mechanics::TriggerEvent> events,
                                                std::size_t& eventCount) const noexcept {
    eventCount = 0;
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (events.size() < slot.triggerEventCount) {
        return HostStatus::capacity;
    }
    std::copy_n(slot.triggerEvents.begin(), slot.triggerEventCount, events.begin());
    eventCount = slot.triggerEventCount;
    return HostStatus::success;
}

/** Copies timer firings from the last fixed service step. */
HostStatus BubbleHost::copy_last_timer_events(WorldHandle handle,
                                              std::span<mechanics::TickTimerEvent> events,
                                              std::size_t& eventCount) const noexcept {
    eventCount = 0;
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    const Storage::WorldSlot& slot = storage_->worlds[slotIndex];
    if (events.size() < slot.timerEventCount) {
        return HostStatus::capacity;
    }
    std::copy_n(slot.timerEvents.begin(), slot.timerEventCount, events.begin());
    eventCount = slot.timerEventCount;
    return HostStatus::success;
}

/** Copies the last safe fallback path retained for one actor. */
HostStatus BubbleHost::read_path(WorldHandle handle,
                                 world::ActorKey actor,
                                 backend::NavigationPath& path) const noexcept {
    path = {};
    const std::size_t slotIndex = find_slot(handle);
    if (slotIndex == kWorldCapacity) {
        return storage_ == nullptr ? HostStatus::unavailable : HostStatus::staleHandle;
    }
    for (const Storage::PathBinding& binding : storage_->worlds[slotIndex].paths) {
        if (binding.occupied && world::equal_actor(binding.actor, actor)) {
            path = binding.path;
            return HostStatus::success;
        }
    }
    return HostStatus::staleHandle;
}

} // namespace sunrise::server::gameplay::physics::host
