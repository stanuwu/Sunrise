#include <cstddef>
#include <limits>

#include "actor_controller_service.h"
#include "controller_internal.h"

namespace sunrise::server::gameplay::physics::controller {

using namespace internal;

/**
 * Binds empty fixed storage to exact backends and one fixed interval.
 * @param physics Physics target source and sink.
 * @param navigation Navigation query source.
 * @param fixedDt Positive fixed interval in seconds.
 * @return True when the interval is valid and binding commits.
 */
bool ActorControllerService::initialize(backend::PhysicsBackend& physics,
                                        backend::NavigationBackend& navigation,
                                        float fixedDt) noexcept {
    if (!finite(fixedDt) || fixedDt <= 0.0F) {
        return false;
    }
    if (initialized_) {
        for (Slot& slot : slots_) {
            if (slot.occupied) {
                static_cast<void>(stop_motion(slot));
            }
        }
    }
    for (Slot& slot : slots_) {
        const std::uint32_t next = next_generation(slot.generation);
        const std::uint32_t generation = next == 0 ? slot.generation : next;
        slot = {};
        slot.generation = generation;
    }
    events_ = {};
    physics_ = &physics;
    navigation_ = &navigation;
    lastTick_ = 0;
    nextEventSequence_ = 1;
    fixedDt_ = fixedDt;
    controllerCount_ = 0;
    eventCount_ = 0;
    initialized_ = true;
    healthy_ = true;
    return true;
}

/** Stops motion, clears fixed storage, and invalidates issued handles. */
void ActorControllerService::shutdown() noexcept {
    if (initialized_) {
        for (Slot& slot : slots_) {
            if (slot.occupied) {
                static_cast<void>(stop_motion(slot));
            }
            const std::uint32_t next = next_generation(slot.generation);
            const std::uint32_t generation = next == 0 ? slot.generation : next;
            slot = {};
            slot.generation = generation;
        }
    }
    events_ = {};
    physics_ = nullptr;
    navigation_ = nullptr;
    lastTick_ = 0;
    nextEventSequence_ = 1;
    fixedDt_ = 0.0F;
    controllerCount_ = 0;
    eventCount_ = 0;
    initialized_ = false;
    healthy_ = true;
}

std::span<const ControllerEvent> ActorControllerService::events() const noexcept {
    return {events_.data(), eventCount_};
}

void ActorControllerService::clear_events() noexcept {
    eventCount_ = 0;
}

std::size_t ActorControllerService::controller_count() const noexcept {
    return controllerCount_;
}

bool ActorControllerService::healthy() const noexcept {
    return healthy_;
}

/**
 * Appends one ordered mechanics outcome to the bounded event buffer.
 * @param slotIndex Exact occupied controller slot.
 * @param tick Fixed tick which owns the event.
 * @param kind Generic mechanics outcome.
 * @param status Controller result mapped by the host.
 * @param navigationStatus Navigation result tied to the outcome.
 * @param position Canonical actor position at the outcome.
 * @return False when fixed event storage or sequence space is exhausted.
 */
bool ActorControllerService::append_event(std::size_t slotIndex,
                                          world::TickId tick,
                                          ControllerEventKind kind,
                                          ControllerStatus status,
                                          backend::NavigationStatus navigationStatus,
                                          world::Vector3 position) noexcept {
    if (slotIndex >= slots_.size() || eventCount_ >= events_.size() || nextEventSequence_ == 0
        || nextEventSequence_ == std::numeric_limits<std::uint64_t>::max()) {
        healthy_ = false;
        return false;
    }
    const Slot& slot = slots_[slotIndex];
    ControllerEvent event{};
    event.controller = {static_cast<std::uint16_t>(slotIndex), slot.generation};
    event.actor = slot.binding.actor;
    event.tick = tick;
    event.sequence = nextEventSequence_++;
    event.position = position;
    event.navigationStatus = navigationStatus;
    event.status = status;
    event.kind = kind;
    events_[eventCount_++] = event;
    return true;
}

/**
 * Cancels a queued target and writes zero velocity to one bound body.
 * @param slot Exact occupied controller slot.
 * @return True when both backend mutations commit.
 */
bool ActorControllerService::stop_motion(Slot& slot) noexcept {
    slot.commandedVelocity = {};
    if (physics_ == nullptr) {
        return false;
    }
    backend::BodyState body{};
    if (!physics_->read_body(slot.binding.body, body)) {
        return false;
    }
    const bool teleported = physics_->teleport_body(slot.binding.body, body.spec.transform);
    const bool stopped = physics_->set_velocity(slot.binding.body, {}, {});
    return teleported && stopped;
}

void ActorControllerService::stop_with_state(Slot& slot, ControllerState state) noexcept {
    static_cast<void>(stop_motion(slot));
    slot.path = {};
    slot.waypointIndex = 0;
    slot.lastWaypointDistance = 0.0F;
    slot.stalledTicks = 0;
    slot.hasProgressSample = false;
    slot.state = state;
    ++slot.revision;
}

} // namespace sunrise::server::gameplay::physics::controller
