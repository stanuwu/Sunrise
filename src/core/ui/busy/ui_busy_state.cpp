#include "ui_busy_state.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../logging/log.h"
#include "busy.h"

namespace sunrise::core::ui::busy {
namespace {

/** Two presents prove the first one reached the screen before the caller stalls the game. */
constexpr std::uint32_t kRequiredPresents = 2;
/** Longest a begin call waits for those presents. */
constexpr ULONGLONG kPresentWaitMilliseconds = 500;
/** A present older than this means the game is not drawing, so there is nothing to wait for. */
constexpr ULONGLONG kPresentIdleMilliseconds = 250;
/** The Present hook is installed just before the first raise, so the first frame gets this long
 * before the overlay is given up on. */
constexpr ULONGLONG kFirstPresentGraceMilliseconds = 250;
/** Longest a deferred caller waits for an overlay frame while the game is presenting. */
constexpr ULONGLONG kEarlyWaitMilliseconds = 1000;
/** An overlay drawn this recently is still on screen, so the next task need not wait for it. */
constexpr ULONGLONG kVisibleRecentlyMilliseconds = 250;
/** One millisecond yields the core between polls of the present counter. */
constexpr DWORD kWaitStepMilliseconds = 1;

std::atomic_uint g_running{};
/** Presents that carried the fully drawn overlay. */
std::atomic_uint32_t g_shownPresents{};
/** Tick of the last finished present, which tells a drawing game from a stalled one. */
std::atomic<ULONGLONG> g_lastPresentTick{};
/** Tick of the last frame that drew the overlay at any alpha. */
std::atomic<ULONGLONG> g_lastVisibleTick{};
/** Thread that presents. It can never wait for its own frames. */
std::atomic<DWORD> g_presentThread{};
/** Raised by a fully drawn overlay frame, taken by the present that follows it. */
std::atomic_bool g_drawPending{};

// One bit per task in that word. A task past the last bit would shift out of range silently.
static_assert(static_cast<unsigned>(Task::count) <= std::numeric_limits<unsigned>::digits);
/** Fixed storage follows the complete task enum. */
constexpr std::size_t kTaskCount = static_cast<std::size_t>(Task::count);

/** Presentation state for one task that must yield before it stalls the presenting caller. */
struct EarlyState {
    std::uint32_t start{};
    ULONGLONG armedTick{};
    bool armed{};
    bool expiryReported{};
};

SRWLOCK g_earlyLock{SRWLOCK_INIT};
std::array<EarlyState, kTaskCount> g_early{};

/** @return The task's single bit in the running mask. */
[[nodiscard]] unsigned bit_of(Task task) noexcept {
    return 1U << static_cast<unsigned>(task);
}

/** @return True while the game presented a frame recently enough to draw the overlay. */
[[nodiscard]] bool drawing() noexcept {
    const ULONGLONG lastPresent = g_lastPresentTick.load(std::memory_order_acquire);
    return lastPresent != 0 && GetTickCount64() - lastPresent <= kPresentIdleMilliseconds;
}

/** @return True while a frame drawn recently still carries the overlay on screen. */
[[nodiscard]] bool on_screen() noexcept {
    const ULONGLONG visible = g_lastVisibleTick.load(std::memory_order_acquire);
    return visible != 0 && GetTickCount64() - visible <= kVisibleRecentlyMilliseconds;
}

/** Blocks until the overlay has reached the screen, the deadline passes, or drawing stops. */
void wait_for_present() noexcept {
    if (GetCurrentThreadId() == g_presentThread.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint32_t start = g_shownPresents.load(std::memory_order_acquire);
    const ULONGLONG deadline = GetTickCount64() + kPresentWaitMilliseconds;
    while (g_shownPresents.load(std::memory_order_acquire) - start < kRequiredPresents) {
        if (GetTickCount64() >= deadline || !drawing()) {
            return;
        }
        Sleep(kWaitStepMilliseconds);
    }
}

} // namespace

/** @param task Work that is running. */
void raise(Task task) noexcept {
    g_running.fetch_or(bit_of(task), std::memory_order_acq_rel);
}

/** @param task Work that has started. */
void begin(Task task) noexcept {
    const unsigned prior = g_running.fetch_or(bit_of(task), std::memory_order_acq_rel);
    // Nested work, and work that resumes inside the fade, inherit an overlay the screen already
    // carries. Only work that opens a cleared overlay waits for one.
    if (prior != 0 || on_screen()) {
        return;
    }
    wait_for_present();
}

/** Raises a task and reports whether the caller must let a frame pass before starting. */
bool raise_early(Task task) noexcept {
    AcquireSRWLockExclusive(&g_earlyLock);
    g_running.fetch_or(bit_of(task), std::memory_order_acq_rel);
    EarlyState& early = g_early[static_cast<std::size_t>(task)];
    const ULONGLONG now = GetTickCount64();
    if (!early.armed) {
        early.start = g_shownPresents.load(std::memory_order_acquire);
        early.armedTick = now;
        early.armed = true;
    }
    const ULONGLONG waited = now - early.armedTick;
    bool waiting = false;
    bool expired = false;
    if (g_lastPresentTick.load(std::memory_order_acquire) == 0) {
        // No frame has passed through the hook yet, which is expected on the call that
        // installed it. The game gets a bounded chance to draw one.
        waiting = waited < kFirstPresentGraceMilliseconds;
    } else if (drawing()) {
        const bool shown =
            g_shownPresents.load(std::memory_order_acquire) - early.start >= kRequiredPresents;
        // Bounded: an overlay that never completes must not defer the caller for the whole run.
        waiting = !shown && waited < kEarlyWaitMilliseconds;
        expired = !shown && !waiting && !early.expiryReported;
        early.expiryReported = early.expiryReported || expired;
    }
    ReleaseSRWLockExclusive(&g_earlyLock);
    if (expired) {
        // The sink takes a process-wide lock, so it is written outside the state lock.
        core::log::write(core::log::Channel::core,
                         core::log::Level::warn,
                         "ev=busy stage=early result=expired reason=no_overlay_frame");
    }
    return waiting;
}

/** @param task Work that has finished. */
void end(Task task) noexcept {
    AcquireSRWLockExclusive(&g_earlyLock);
    g_running.fetch_and(~bit_of(task), std::memory_order_acq_rel);
    g_early[static_cast<std::size_t>(task)] = {};
    ReleaseSRWLockExclusive(&g_earlyLock);
}

/** Records one finished present. */
void confirm_presented() noexcept {
    g_lastPresentTick.store(GetTickCount64(), std::memory_order_release);
    if (g_drawPending.exchange(false, std::memory_order_acq_rel)) {
        g_shownPresents.fetch_add(1, std::memory_order_acq_rel);
    }
}

namespace internal {

/** @return Mask of started tasks. */
unsigned running() noexcept {
    return g_running.load(std::memory_order_acquire);
}

/** @param threadId Thread that draws frames. */
void set_present_thread(DWORD threadId) noexcept {
    g_presentThread.store(threadId, std::memory_order_release);
}

/** @param complete True when the frame drew the overlay at full alpha. */
void record_drawn(bool complete) noexcept {
    g_lastVisibleTick.store(GetTickCount64(), std::memory_order_release);
    if (complete) {
        g_drawPending.store(true, std::memory_order_release);
    }
}

} // namespace internal
} // namespace sunrise::core::ui::busy
