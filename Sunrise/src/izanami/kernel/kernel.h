#pragma once

#include "component_registry.h"
#include "event_bus.h"
#include "service_registry.h"

namespace sunrise::izanami::kernel {

class Kernel final {
public:
    void initialize_defaults_once();
    void shutdown();

    [[nodiscard]] ServiceRegistry& services() noexcept;
    [[nodiscard]] EventBus& events() noexcept;
    [[nodiscard]] ComponentRegistry& components() noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    ServiceRegistry services_{};
    EventBus events_{};
    ComponentRegistry components_{};
    bool initialized_{};
};

[[nodiscard]] Kernel& kernel() noexcept;

} // namespace sunrise::izanami::kernel