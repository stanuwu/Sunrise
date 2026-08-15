#include "network_call_quiescence.h"

#include <algorithm>

namespace sunrise::client::hooks::network::quiescence {
namespace {

/** 10 ms slices close the wake race before the decrement, without busy polling. */
constexpr DWORD kWaitSliceMilliseconds = 10;

} // namespace

/** Reads one outermost-call counter without tearing. */
LONG active_calls(volatile LONG& counter) noexcept {
    return InterlockedCompareExchange(&counter, 0, 0);
}

/** Waits without spinning until a counter reaches zero. */
bool wait_for_idle(volatile LONG& counter, DWORD timeoutMilliseconds) noexcept {
    const ULONGLONG started = GetTickCount64();
    LONG observed = active_calls(counter);
    while (observed != 0) {
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeoutMilliseconds) {
            return false;
        }
        const auto remaining = static_cast<DWORD>(
            std::min<ULONGLONG>(timeoutMilliseconds - elapsed, kWaitSliceMilliseconds));
        if (WaitOnAddress(&counter, &observed, sizeof observed, remaining) == FALSE
            && GetLastError() != ERROR_TIMEOUT) {
            return false;
        }
        observed = active_calls(counter);
    }
    return true;
}

#if defined(SUNRISE_BAP_HOOK_TEST)
/** @param gate Gate armed before the next call is counted in. */
void arm(PauseGate& gate) noexcept {
    (void)InterlockedExchange(&gate.entered, 0);
    (void)InterlockedExchange(&gate.released, 0);
    (void)InterlockedExchange(&gate.armed, 1);
}

/** Waits for an armed call to reach the pause. */
bool wait_for_entry(PauseGate& gate, DWORD timeoutMilliseconds) noexcept {
    const ULONGLONG started = GetTickCount64();
    while (InterlockedCompareExchange(&gate.entered, 0, 0) == 0) {
        if (GetTickCount64() - started >= timeoutMilliseconds) {
            return false;
        }
        Sleep(1);
    }
    return true;
}

/** @param gate Gate whose paused call is let go. */
void release(PauseGate& gate) noexcept {
    (void)InterlockedExchange(&gate.released, 1);
}

/** Takes an armed gate and says whether this call must pause. */
bool consume_arm(PauseGate& gate) noexcept {
    if (InterlockedCompareExchange(&gate.armed, 0, 1) != 1) {
        return false;
    }
    (void)InterlockedExchange(&gate.entered, 1);
    return true;
}

/** @param gate Gate being held. @return True while the holder must keep spinning. */
bool held(const PauseGate& gate) noexcept {
    return gate.released == 0;
}
#endif

} // namespace sunrise::client::hooks::network::quiescence
