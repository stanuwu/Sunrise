#pragma once

#include <Windows.h>

#include "../platform.h"

namespace sunrise::client::hooks::network::coordinator {

/** Picks which consumer pointer is copied for one replacement call. */
enum class ConsumerKind {
    none,
    http,
    bap,
};

/** State copied at ingress and kept until egress. */
struct CallLease final {
    void* original{};
    sunrise::client::network::HttpConsumer httpConsumer{};
    sunrise::client::network::BapConsumer bapConsumer{};
    bool accepting{};
};

/** The exact ingress ABI every base-network replacement body calls. */
using CallIngress = void (*)(CallLease&, HookSlot, ConsumerKind) noexcept;
/** The exact egress ABI every base-network replacement body calls. */
using CallEgress = void (*)() noexcept;

extern const CallIngress g_callIngress;
extern const CallEgress g_callEgress;

/** @return The internal-linkage ingress body, kept safe while the detour is removed. */
[[nodiscard]] void* ingress_entry_point() noexcept;

/** @return The internal-linkage egress body, kept safe while the detour is removed. */
[[nodiscard]] void* egress_entry_point() noexcept;

/** @return True only when no outer base-network call is running. */
[[nodiscard]] bool idle() noexcept;

/**
 * Waits without spinning until every outer base-network call leaves.
 * @param timeoutMilliseconds Longest time to wait.
 * @return True when no tracked replacement is still running.
 */
[[nodiscard]] bool wait_for_idle(DWORD timeoutMilliseconds) noexcept;

/** @return How many outermost base-network calls are running now, process-wide. */
[[nodiscard]] LONG active_calls() noexcept;

/** @return True while the current thread owns a base-network call lease. */
[[nodiscard]] bool current_thread_active() noexcept;

#if defined(SUNRISE_BAP_HOOK_TEST)
namespace testing {

/** Arms a short pause before the next base-network call is counted in. */
void arm_ingress_pause() noexcept;

/** @return True when the armed call reaches the protected ingress pause in time. */
[[nodiscard]] bool wait_for_ingress_pause(DWORD timeoutMilliseconds) noexcept;

/** Releases the ingress pause. */
void release_ingress_pause() noexcept;

} // namespace testing
#endif

} // namespace sunrise::client::hooks::network::coordinator
