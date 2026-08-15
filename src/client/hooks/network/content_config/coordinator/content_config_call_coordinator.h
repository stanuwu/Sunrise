#pragma once

#include <Windows.h>

#include "../internal.h"

namespace sunrise::client::hooks::network::content_config::coordinator {

/** State copied at ingress and kept until egress. */
struct CallLease final {
    void* original{};
    ContentFetch fetch{};
    bool accepting{};
    bool tracked{};
};

/** The exact ingress ABI every ContentConfig replacement body calls. */
using CallIngress = void (*)(CallLease&, HookSlot) noexcept;
/** The exact egress ABI every ContentConfig replacement body calls. */
using CallEgress = void (*)(bool) noexcept;

extern const CallIngress g_callIngress;
extern const CallEgress g_callEgress;

/** @return The internal-linkage ingress body, kept safe while the detour is removed. */
[[nodiscard]] void* ingress_entry_point() noexcept;

/** @return The internal-linkage egress body, kept safe while the detour is removed. */
[[nodiscard]] void* egress_entry_point() noexcept;

/** @return True only when no outer ContentConfig call is running. */
[[nodiscard]] bool idle() noexcept;

/**
 * Waits without spinning until every outermost replacement call leaves.
 * @param timeoutMilliseconds Longest time to wait.
 * @return True when no tracked replacement is still running.
 */
[[nodiscard]] bool wait_for_idle(DWORD timeoutMilliseconds) noexcept;

/** @return How many outermost replacement calls are running now, process-wide. */
[[nodiscard]] LONG active_calls() noexcept;

#if defined(SUNRISE_BAP_HOOK_TEST)
namespace testing {

/** Arms a short pause before the next ContentConfig call is counted in. */
void arm_ingress_pause() noexcept;

/** @return True when the armed call reaches the protected ingress pause in time. */
[[nodiscard]] bool wait_for_ingress_pause(DWORD timeoutMilliseconds) noexcept;

/** Releases the ingress pause. */
void release_ingress_pause() noexcept;

} // namespace testing
#endif

} // namespace sunrise::client::hooks::network::content_config::coordinator
