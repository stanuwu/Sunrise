#include <Windows.h>

#include <array>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../process/freeze/client_process_freeze.h"
#include "../items/packages/build.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::content::investment {
namespace {

SRWLOCK g_refreshLock{SRWLOCK_INIT};

/** One line reports the freeze, so a run that could not hold the game is visible. */
constexpr std::size_t kLineLimit = 96;

/**
 * @return True when every persistent mapping domain is fully published.
 * The destination layouts and spawn sets belong here even though they are not equipment mappings.
 * This is the only caller of the package pass, so a domain left out of this test stops being
 * extracted once the others finish, and the cache can then never be written.
 */
[[nodiscard]] bool ready() noexcept {
    return state::build_data::named_catalog_ready() && state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::scenario_layouts_ready() && state::build_data::spawn_sets_ready()
           && state::build_data::hash_names_ready()
           && state::build_data::investment_constants_ready();
}

/**
 * Reports the outcome of one freeze attempt.
 * @param frozen True when the game was held.
 * @param threadCount Threads that were suspended.
 */
void report_freeze(bool frozen, std::size_t threadCount) noexcept {
    std::array<char, kLineLimit> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=extract stage=freeze result=%s threads=%zu",
                                      frozen ? "ok" : "fail",
                                      threadCount);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    // The pass runs in slices, so a working freeze reports at debug and only a failure is loud.
    core::log::write(core::log::Channel::client,
                     frozen ? core::log::Level::debug : core::log::Level::warn,
                     std::string_view(line.data(), length));
}

} // namespace

/** @return True when the next refresh slice must hold the process for a package sweep. */
bool requires_process_freeze() noexcept {
    return !state::build_data::item_definitions_ready() && items::packages::readable();
}

/** Publishes every installed equipment mapping domain. */
bool refresh() noexcept {
    if (ready()) {
        // The same lock as the extraction path. A cache write holds its own lock across file
        // calls, so a held thread stopped inside one would deadlock the freeze below.
        AcquireSRWLockExclusive(&g_refreshLock);
        const bool persisted = state::build_data::persist();
        // Nothing reads a package again until the next boot, so the open files and the held
        // tables go back now rather than at process exit.
        middleware::content::packages::reader::release_caches();
        ReleaseSRWLockExclusive(&g_refreshLock);
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
        return persisted;
    }

    AcquireSRWLockExclusive(&g_refreshLock);
    // Only the item sweep holds the game, and only once the block keys exist. The destination and
    // spawn-set passes after it are tens of thousands of tag reads over many slices, so the
    // overlay covers the whole pass: without it the longest stall of the boot has nothing on
    // screen.
    const bool sweeping = requires_process_freeze();
    process::freeze::Held held{};
    bool frozen = false;
    std::size_t heldThreads = 0;
    if (sweeping) {
        // The overlay reaches the screen before the freeze stops the frame loop. A held game
        // cannot time its connection out, which a slow disk otherwise causes here.
        core::ui::busy::begin(core::ui::busy::Task::contentExtraction);
        frozen = process::freeze::hold(held);
        heldThreads = held.count;
    } else {
        core::ui::busy::raise(core::ui::busy::Task::contentExtraction);
    }
    // The package pass owns the item table and must not wait on runtime content lookups.
    (void)items::packages::build();
    const bool complete = ready() && state::build_data::persist();
    process::freeze::release(held);
    // The overlay ends with the work, not with the slice, so it spans every retry the pass needs.
    if (complete) {
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
    }
    ReleaseSRWLockExclusive(&g_refreshLock);
    if (sweeping) {
        report_freeze(frozen, heldThreads);
    }
    return complete;
}

} // namespace sunrise::client::content::investment
