#include "event_bus.h"

#include <algorithm>

namespace sunrise::izanami::kernel {

/** Adds one static event subscriber. */
bool EventBus::subscribe(EventCallback callback) {
    if (callback == nullptr) {
        return false;
    }
    const std::lock_guard lock{mutex_};
    const auto found = std::find(subscribers_.begin(), subscribers_.end(), callback);
    if (found != subscribers_.end()) {
        return false;
    }
    subscribers_.push_back(callback);
    return true;
}

/** Removes one event subscriber. */
bool EventBus::unsubscribe(EventCallback callback) {
    const std::lock_guard lock{mutex_};
    const auto found = std::find(subscribers_.begin(), subscribers_.end(), callback);
    if (found == subscribers_.end()) {
        return false;
    }
    subscribers_.erase(found);
    return true;
}

/** Publishes one event to the current subscribers. */
void EventBus::publish(const Event& event) noexcept {
    std::vector<EventCallback> subscribers;
    {
        const std::lock_guard lock{mutex_};
        subscribers = subscribers_;
        ++published_;
    }
    for (EventCallback callback : subscribers) {
        callback(event);
    }
}

/** @return Number of current event subscribers. */
std::size_t EventBus::subscriber_count() const {
    const std::lock_guard lock{mutex_};
    return subscribers_.size();
}

/** @return Number of events published since the last clear. */
std::size_t EventBus::published_count() const noexcept {
    return published_;
}

/** Clears subscribers and event counters. */
void EventBus::clear() {
    const std::lock_guard lock{mutex_};
    subscribers_.clear();
    published_ = 0;
}

} // namespace sunrise::izanami::kernel