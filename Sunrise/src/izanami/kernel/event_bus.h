#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../core/ids.h"

namespace sunrise::izanami::kernel {

enum class EventKind : std::uint8_t {
    objectCreated,
    objectDeleted,
    transformChanged,
    selectionChanged,
    capabilityChanged,
};

struct Event {
    EventKind kind{EventKind::objectCreated};
    core::ForgeUUID object{};
};

using EventCallback = void (*)(const Event&) noexcept;

class EventBus final {
public:
    [[nodiscard]] bool subscribe(EventCallback callback);
    [[nodiscard]] bool unsubscribe(EventCallback callback);
    void publish(const Event& event) noexcept;
    [[nodiscard]] std::size_t subscriber_count() const;
    [[nodiscard]] std::size_t published_count() const noexcept;
    void clear();

private:
    mutable std::mutex mutex_{};
    std::vector<EventCallback> subscribers_{};
    std::size_t published_{};
};

} // namespace sunrise::izanami::kernel