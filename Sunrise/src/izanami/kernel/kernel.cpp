#include "kernel.h"

#include "../editor/workspace/editor_workspace.h"
#include "../runtime/runtime_adapter.h"

namespace sunrise::izanami::kernel {

namespace {

enum ComponentType : std::uint32_t {
    transformComponent = 0x1001,
    nativeBindingComponent = 0x1002,
    scriptBindingComponent = 0x1003,
};

Kernel g_kernel;

} // namespace

/** Registers the stable built-in Izanami services and component types once. */
void Kernel::initialize_defaults_once() {
    if (initialized_) {
        return;
    }
    initialized_ = true;

    (void)services_.register_service("izanami.workspace", &editor::workspace::workspace());
    (void)services_.register_service("izanami.runtime", &runtime::unsupported_runtime());
    (void)components_.register_component({transformComponent, 1, "Transform"});
    (void)components_.register_component({nativeBindingComponent, 1, "Native Binding"});
    (void)components_.register_component({scriptBindingComponent, 1, "Script Binding"});
}

/** Clears all process-local kernel registries. */
void Kernel::shutdown() {
    services_.clear();
    events_.clear();
    components_.clear();
    initialized_ = false;
}

ServiceRegistry& Kernel::services() noexcept {
    return services_;
}

EventBus& Kernel::events() noexcept {
    return events_;
}

ComponentRegistry& Kernel::components() noexcept {
    return components_;
}

bool Kernel::initialized() const noexcept {
    return initialized_;
}

/** @return Process-local Izanami kernel. */
Kernel& kernel() noexcept {
    return g_kernel;
}

} // namespace sunrise::izanami::kernel