#include "../coordinator/network_call_coordinator.h"
#include "../platform.h"
#include "internal.h"

namespace sunrise::client::hooks::network::http {
namespace {

/** Exact protected replacement body for the common HTTP executor. */
__declspec(noinline) std::int64_t __fastcall execute_request_body(void* wrapper,
                                                                  std::byte* descriptor) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::httpExecuteRequest, coordinator::ConsumerKind::http);
    const std::int64_t result = route_descriptor(lease, wrapper, descriptor);
    coordinator::g_callEgress();
    return result;
}

} // namespace

/** @return The body itself. Internal linkage, so it cannot be a linker thunk. */
void* execute_request_entry_point() noexcept {
    return reinterpret_cast<void*>(&execute_request_body);
}

} // namespace sunrise::client::hooks::network::http
