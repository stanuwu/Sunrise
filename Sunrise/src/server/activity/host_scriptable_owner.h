#pragma once

#include "host_runtime_internal.h"

namespace sunrise::server::activity::host::ownership {
/** True while the instance's view has an active, queued scriptable-override output. */
[[nodiscard]] inline bool pending(const detail::Instance* instance) noexcept {
    return instance != nullptr && instance->view.active && instance->view.outputPending
           && instance->view.outputKind == OutputKind::scriptableOverride
           && instance->pendingScriptable.revision != 0;
}
/** A read from another native client cannot consume or cancel an authorized output. */
[[nodiscard]] inline bool readable(const detail::Instance* instance,
                                   std::uint64_t generation) noexcept {
    return generation != 0 && pending(instance)
           && (instance->pendingScriptable.expectedActivityClientGeneration == 0
               || instance->pendingScriptable.expectedActivityClientGeneration == generation);
}
/** A retiring generation only withdraws controls that explicitly name it. */
[[nodiscard]] inline bool owns(const detail::Instance* instance,
                               std::uint64_t generation) noexcept {
    return generation != 0 && pending(instance)
           && instance->pendingScriptable.expectedActivityClientGeneration == generation;
}
/** Retires unread native controls and reports exactly how much queue accounting to release. */
[[nodiscard]] inline std::size_t retire(std::span<detail::PendingInput> unread,
                                        const state::activity::SessionBinding& binding,
                                        std::uint64_t generation) noexcept {
    if (!generation) {
        return 0;
    }
    std::size_t count = 0;
    for (auto& input : unread) {
        if (input.kind != detail::PendingKind::scriptableControl
            || !state::activity::same_binding(input.scriptableControl.binding, binding)
            || input.scriptableControl.expectedActivityClientGeneration != generation) {
            continue;
        }
        input.scriptableControl = {};
        input.kind = detail::PendingKind::discardedControl;
        ++count;
    }
    return count;
}
} // namespace sunrise::server::activity::host::ownership
