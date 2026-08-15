#include "network_call_coordinator.h"

#include "../network_call_quiescence.h"

namespace sunrise::client::hooks::network {

volatile LONG g_activeCalls{};

} // namespace sunrise::client::hooks::network

namespace sunrise::client::hooks::network::coordinator {
namespace {

thread_local unsigned g_callDepth{};

#if defined(SUNRISE_BAP_HOOK_TEST)
quiescence::PauseGate g_pauseGate;

#endif

/**
 * Counts the outer call in, then copies lifecycle state under the shared lock.
 * @param lease Receives the original pointer and the admission flags.
 * @param slot Replacement slot entered by this thread.
 * @param consumer Which consumer pointer this replacement needs.
 */
__declspec(noinline) void
ingress_body(CallLease& lease, HookSlot slot, ConsumerKind consumer) noexcept {
#if defined(SUNRISE_BAP_HOOK_TEST)
    // The spin stays here, not in a helper. Detour removal refuses while a thread sits inside
    // this body, and the paused call is what proves it.
    if (quiescence::consume_arm(g_pauseGate)) {
        for (unsigned attempt = 0;
             attempt < quiescence::kIngressPauseAttempts && quiescence::held(g_pauseGate);
             ++attempt) {
            YieldProcessor();
        }
    }
#endif
    if (g_callDepth == 0) {
        (void)InterlockedIncrement(&g_activeCalls);
    }

    AcquireSRWLockShared(&g_lock);
    lease.original = original<void*>(slot);
    lease.accepting = g_accepting[static_cast<std::size_t>(slot)];
    if (consumer == ConsumerKind::http) {
        lease.httpConsumer = g_httpConsumer.load(std::memory_order_acquire);
    } else if (consumer == ConsumerKind::bap) {
        lease.bapConsumer = g_bapConsumer.load(std::memory_order_acquire);
    }
    ++g_callDepth;
    ReleaseSRWLockShared(&g_lock);
}

/** Counts the outer call out. This is the last step before the protected return. */
__declspec(noinline) void egress_body() noexcept {
    if (g_callDepth == 1) {
        // This non-leaf call keeps unwind data present while the active token is still held.
        WakeByAddressAll(const_cast<LONG*>(&g_activeCalls));
    }
    --g_callDepth;
    if (g_callDepth == 0) {
        (void)InterlockedDecrement(&g_activeCalls);
    }
}

} // namespace

const CallIngress g_callIngress = &ingress_body;
const CallEgress g_callEgress = &egress_body;

/** @return The internal-linkage ingress body, kept safe while the detour is removed. */
void* ingress_entry_point() noexcept {
    return reinterpret_cast<void*>(&ingress_body);
}

/** @return The internal-linkage egress body, kept safe while the detour is removed. */
void* egress_entry_point() noexcept {
    return reinterpret_cast<void*>(&egress_body);
}

/** @return True only when no outer base-network call is running. */
bool idle() noexcept {
    return active_calls() == 0;
}

/** Waits without spinning until every outer base-network call leaves. */
bool wait_for_idle(DWORD timeoutMilliseconds) noexcept {
    return quiescence::wait_for_idle(g_activeCalls, timeoutMilliseconds);
}

/** @return How many outermost base-network calls are running now, process-wide. */
LONG active_calls() noexcept {
    return quiescence::active_calls(g_activeCalls);
}

/** @return True while the current thread owns a base-network call lease. */
bool current_thread_active() noexcept {
    return g_callDepth != 0;
}

#if defined(SUNRISE_BAP_HOOK_TEST)
namespace testing {

/** Arms a short pause before the next base-network call is counted in. */
void arm_ingress_pause() noexcept {
    quiescence::arm(g_pauseGate);
}

/** @return True when the armed call reaches the protected ingress pause in time. */
bool wait_for_ingress_pause(DWORD timeoutMilliseconds) noexcept {
    return quiescence::wait_for_entry(g_pauseGate, timeoutMilliseconds);
}

/** Releases the ingress pause. */
void release_ingress_pause() noexcept {
    quiescence::release(g_pauseGate);
}

} // namespace testing
#endif

} // namespace sunrise::client::hooks::network::coordinator
