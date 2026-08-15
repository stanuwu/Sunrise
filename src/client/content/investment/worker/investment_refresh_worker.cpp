#include <Windows.h>

#include "../../../../core/ui/busy/busy.h"
#include "../../../targets/game/content.h"
#include "../../diagnostics/content_readiness_report.h"
#include "../internal.h"
#include "../runtime.h"
#include "../worker.h"

namespace sunrise::client::content::investment::worker {
namespace {

/**
 * Delay between bounded refresh slices.
 * A slice runs on every pump. The extraction is hundreds of slices and each bounds its own length,
 * so a delay on top only added waiting: at 50 ms it was most of what the boot spent extracting.
 */
constexpr std::uint64_t kRefreshIntervalMilliseconds = 0;

SRWLOCK g_lifecycleLock{SRWLOCK_INIT};
bool g_accepting{};
bool g_complete{};
bool g_overlayPending{};
std::uint64_t g_nextEligible{};

} // namespace

/** Allows cooperative investment refresh slices on the caller-owned game thread. */
void activate() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    g_accepting = true;
    g_complete = false;
    g_overlayPending = false;
    g_nextEligible = 0;
    sunrise::core::ui::busy::end(sunrise::core::ui::busy::Task::contentExtraction);
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

/** Runs one due bounded refresh slice on the caller-owned game thread. */
void service(std::uint64_t nowMilliseconds) noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    if (!g_accepting || g_complete || !sunrise::client::targets::game::content::is_resolved()
        || nowMilliseconds < g_nextEligible) {
        ReleaseSRWLockExclusive(&g_lifecycleLock);
        return;
    }
    g_nextEligible = nowMilliseconds + kRefreshIntervalMilliseconds;

    if (sunrise::client::content::investment::requires_process_freeze()) {
        g_overlayPending = true;
        if (sunrise::core::ui::busy::raise_early(
                sunrise::core::ui::busy::Task::contentExtraction)) {
            ReleaseSRWLockExclusive(&g_lifecycleLock);
            return;
        }
    } else if (g_overlayPending) {
        // A stale preflight must not leave a task raised after another path publishes the rows.
        sunrise::core::ui::busy::end(sunrise::core::ui::busy::Task::contentExtraction);
        g_overlayPending = false;
    }

    g_complete = sunrise::client::content::investment::refresh();
    sunrise::client::content::diagnostics::report_readiness();
    g_overlayPending = false;
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

/** Stops taking refresh slices and clears the pending overlay. */
void reset() noexcept {
    AcquireSRWLockExclusive(&g_lifecycleLock);
    g_accepting = false;
    g_complete = false;
    g_overlayPending = false;
    g_nextEligible = 0;
    sunrise::core::ui::busy::end(sunrise::core::ui::busy::Task::contentExtraction);
    ReleaseSRWLockExclusive(&g_lifecycleLock);
}

} // namespace sunrise::client::content::investment::worker
