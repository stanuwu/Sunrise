#include <atomic>

#include "../../../state/build_data/nodes/node_persistence.h"
#include <Windows.h>

#include "../../../core/ui/busy/busy.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../items/packages/build.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::content::investment {
namespace {

SRWLOCK g_refreshLock{SRWLOCK_INIT};

/**
 * @return True when every persistent mapping domain is fully published.
 * The destination layouts and spawn sets belong here even though they are not equipment mappings.
 * This is the only caller of the package pass, so a domain left out of this test stops being
 * extracted once the others finish, and the cache can then never be written.
 */
[[nodiscard]] bool ready() noexcept {
    return state::build_data::named_catalog_ready() && state::build_data::item_definitions_ready()
           && state::build_data::collectible_definitions_ready()
           && state::build_data::material_requirement_sets_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::socket_plug_rules_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::socket_entry_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::scenario_layouts_ready() && state::build_data::spawn_sets_ready()
           && state::build_data::hash_names_ready()
           && state::build_data::investment_constants_ready();
}

} // namespace

/** @return True when the next refresh slice needs a visible overlay for a package sweep. */
bool requires_package_sweep() noexcept {
    return !state::build_data::item_definitions_ready() && items::packages::readable();
}

/** Publishes every installed equipment mapping domain. */
bool refresh() noexcept {
    // The node table is kept in its own file and is not part of the build data cache. Publishing it
    // here, before the gate below decides whether the package pass runs, is what lets a warm start
    // count categories: the gate itself is left exactly as it was, so no pass can be made to repeat.
    // Latches on success, not on the attempt: an early call is refused rather than failed, so
    // giving up after one try is what left the table empty.
    static std::atomic<bool> nodesPublished{false};
    if (!nodesPublished.load(std::memory_order_relaxed)
        && state::build_data::nodes::load_and_publish()) {
        nodesPublished.store(true, std::memory_order_relaxed);
    }
    if (ready()) {
        // The same lock as the extraction path. A cache write holds its own lock across file
        // calls, so a held thread stopped inside one would deadlock the freeze below.
        AcquireSRWLockExclusive(&g_refreshLock);
        const bool persisted = state::ensure_profile_item_identities()
                               && state::ensure_character_subclasses()
                               && state::build_data::persist();
        // Nothing reads a package again until the next boot, so the open files and the held
        // tables go back now rather than at process exit.
        middleware::content::packages::reader::release_caches();
        ReleaseSRWLockExclusive(&g_refreshLock);
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
        return persisted;
    }

    AcquireSRWLockExclusive(&g_refreshLock);
    // The package pass creates parallel readers. Suspending the client while those threads start
    // can block their DLL thread-attach work behind a suspended owner, so the visible preflight
    // runs one frame early and extraction proceeds with the process live.
    core::ui::busy::raise(core::ui::busy::Task::contentExtraction);
    // The package pass owns the item table and must not wait on runtime content lookups.
    (void)items::packages::build();
    const bool domainsReady = ready();
    const bool complete = domainsReady && state::ensure_profile_item_identities()
                          && state::ensure_character_subclasses() && state::build_data::persist();
    // The overlay ends with the work, not with the slice, so it spans every retry the pass needs.
    if (complete) {
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
    }
    ReleaseSRWLockExclusive(&g_refreshLock);
    return complete;
}

} // namespace sunrise::client::content::investment
