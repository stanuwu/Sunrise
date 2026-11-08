#include <Windows.h>

#include "../../../../core/ui/busy/busy.h"
#include "../../../../server/bap/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../targets/game/content.h"
#include "../../diagnostics/content_readiness_report.h"
#include "../internal.h"
#include "../runtime.h"
#include "../worker.h"
#include "core/threading/data_mutex.h"

namespace sunrise::client::content::investment::worker {
namespace {

/**
 * Delay between bounded refresh slices.
 * Each slice bounds its own length, so a delay on top of that is pure boot latency.
 */
constexpr std::uint64_t kRefreshIntervalMilliseconds = 0;

struct Lifecycle {
    bool accepting{};
    bool complete{};
    bool overlayPending{};
    std::uint64_t nextEligible{};
};

core::threading::DataMutex<Lifecycle> g_lifecycle{};

} // namespace

/** Allows cooperative investment refresh slices on the caller-owned game thread. */
void activate() noexcept {
    g_lifecycle.lock([](Lifecycle& lifecycle) {
        lifecycle.accepting = true;
        lifecycle.complete = false;
        lifecycle.overlayPending = false;
        lifecycle.nextEligible = 0;
        sunrise::core::ui::busy::end(sunrise::core::ui::busy::Task::contentExtraction);
    });
}

/**
 * Runs one due bounded refresh slice on the caller-owned game thread.
 * @param nowMilliseconds Monotonic tick count that gates the next slice.
 */
void service(std::uint64_t nowMilliseconds) noexcept {
    const bool perksPublished = g_lifecycle.lock([nowMilliseconds](Lifecycle& lifecycle) {
        if (!lifecycle.accepting || lifecycle.complete
            || !sunrise::client::targets::game::content::is_resolved()
            || nowMilliseconds < lifecycle.nextEligible) {
            return false;
        }
        lifecycle.nextEligible = nowMilliseconds + kRefreshIntervalMilliseconds;

        if (sunrise::client::content::investment::requires_package_sweep()) {
            lifecycle.overlayPending = true;
            if (sunrise::core::ui::busy::raise_early(
                    sunrise::core::ui::busy::Task::contentExtraction)) {
                return false;
            }
        } else if (lifecycle.overlayPending) {
            // A stale preflight must not leave a task raised after another path publishes the rows.
            sunrise::core::ui::busy::end(sunrise::core::ui::busy::Task::contentExtraction);
            lifecycle.overlayPending = false;
        }

        const bool perksWalkable = sunrise::state::season_perks_walkable();
        lifecycle.complete = sunrise::client::content::investment::refresh();
        sunrise::client::content::diagnostics::report_readiness();
        lifecycle.overlayPending = false;
        return !perksWalkable && sunrise::state::season_perks_walkable();
    });
    // The Server takes this lock while holding its own, so it is told only after release.
    if (perksPublished) {
        sunrise::server::bap::grant_published_season_perks();
    }
}

/** Stops taking refresh slices and clears the pending overlay. */
void reset() noexcept {
    g_lifecycle.lock([](Lifecycle& lifecycle) {
        lifecycle.accepting = false;
        lifecycle.complete = false;
        lifecycle.overlayPending = false;
        lifecycle.nextEligible = 0;
        sunrise::core::ui::busy::end(sunrise::core::ui::busy::Task::contentExtraction);
    });
}

/** Makes the next due pump take another refresh slice even though a prior one completed. */
void request_slice() noexcept {
    g_lifecycle.lock([](Lifecycle& lifecycle) {
        if (lifecycle.accepting) {
            lifecycle.complete = false;
            lifecycle.nextEligible = 0;
        }
    });
}

} // namespace sunrise::client::content::investment::worker
