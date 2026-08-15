#include "../coordinator/content_config_call_coordinator.h"
#include "../gate/content_config_signature_gate.h"
#include "../internal.h"
#include "../lifecycle/content_config_hook_storage.h"

namespace sunrise::client::hooks::network::content_config::tick {
namespace {

/**
 * Keeps the manifest signature gate off, then runs the native tick. The Client drives the whole
 * content sequence: it asks for its URL and token, makes its own fetch, and moves its own phase
 * on. Nothing here writes Client state.
 * @param self Native content state-machine object.
 */
void update_impl(const coordinator::CallLease& lease, void* self) noexcept {
    const auto original = reinterpret_cast<ContentTick>(lease.original);
    if (lease.accepting) {
        // The Client's own content init can put the gate back after install, and a live gate
        // rejects the raw body its fetch downloads.
        (void)gate::reassert(lifecycle::g_gateOwnership);
    }
    if (original != nullptr) {
        original(self);
    }
}

/** Exact protected replacement body for the local ContentConfig tick. */
__declspec(noinline) void __fastcall update_body(void* self) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::contentTick);
    update_impl(lease, self);
    coordinator::g_callEgress(lease.tracked);
}

} // namespace

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* entry_point() noexcept {
    return reinterpret_cast<void*>(&update_body);
}

} // namespace sunrise::client::hooks::network::content_config::tick
