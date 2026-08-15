#pragma once

#include <Windows.h>

namespace sunrise::client::hooks::network::quiescence {

/**
 * Reads one outermost-call counter without tearing.
 * @param counter Counter the coordinator counts its outermost calls in.
 * @return Current count.
 */
[[nodiscard]] LONG active_calls(volatile LONG& counter) noexcept;

/**
 * Waits without spinning until a counter reaches zero. Detour removal waits on this, so it must
 * not busy poll and must not return early. A false return leaves callers inside bodies that are
 * about to be unmapped.
 * @param counter Counter the coordinator counts its outermost calls in.
 * @param timeoutMilliseconds Longest total wait.
 * @return True when the counter reached zero inside the timeout.
 */
[[nodiscard]] bool wait_for_idle(volatile LONG& counter, DWORD timeoutMilliseconds) noexcept;

#if defined(SUNRISE_BAP_HOOK_TEST)
/** One coordinator's ingress pause, armed by a test to hold a call inside the body. */
struct PauseGate final {
    volatile LONG armed{};
    volatile LONG entered{};
    volatile LONG released{1};
};

/** @param gate Gate armed before the next call is counted in. */
void arm(PauseGate& gate) noexcept;

/**
 * Waits for an armed call to reach the pause.
 * @param gate Armed gate.
 * @param timeoutMilliseconds Longest total wait.
 * @return True when a call entered the pause in time.
 */
[[nodiscard]] bool wait_for_entry(PauseGate& gate, DWORD timeoutMilliseconds) noexcept;

/** @param gate Gate whose paused call is let go. */
void release(PauseGate& gate) noexcept;

/** The pause ends on its own, even if the test that armed it exits early. */
inline constexpr unsigned kIngressPauseAttempts = 1'000'000'000;

/**
 * Takes an armed gate and says whether this call must pause. The spin must stay in the caller:
 * detour removal refuses while a thread sits inside the protected body, and the test needs that,
 * so parking the thread in another function would let removal succeed.
 * @return True when this call is the one to hold.
 */
[[nodiscard]] bool consume_arm(PauseGate& gate) noexcept;

/** @param gate Gate being held. @return True while the holder must keep spinning. */
[[nodiscard]] bool held(const PauseGate& gate) noexcept;
#endif

} // namespace sunrise::client::hooks::network::quiescence
