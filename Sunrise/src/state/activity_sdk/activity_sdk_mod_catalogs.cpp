/** Activity SDK packs that mods ship for their own destinations, beside the installed pack. */

#include <algorithm>
#include <atomic>
#include <string_view>

#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "internal.h"
#include "validation_internal.h"

namespace sunrise::state::activity_sdk {
namespace {

/** Only the low byte of a state's authored word is the public flag. */
constexpr std::uint32_t kPublicFlagMask = 0xFFU;

std::atomic<std::shared_ptr<const std::vector<Snapshot>>> g_mods{};

/** Borrows the package-name bytes after checking their fixed storage bound. */
[[nodiscard]] std::string_view
package_name(const activity::destination::DestinationSelection& destination) noexcept {
    if (destination.packageNameLength > destination.packageName.size()) {
        return {};
    }
    return {reinterpret_cast<const char*>(destination.packageName.data()),
            destination.packageNameLength};
}

/** @return True when a row range stays inside a section of `size` rows. */
[[nodiscard]] constexpr bool range(format::Range value, std::size_t size) noexcept {
    return value.first <= size && value.count <= size - value.first;
}

/** Checks every activity, scenario and topology row index a resolve and a roster follow. */
[[nodiscard]] bool valid_rows(const Catalog& catalog) noexcept {
    const auto scenarios = catalog.scenarios();
    const auto bubbles = catalog.bubbles();
    const auto states = catalog.states();
    const auto objects = catalog.objects();
    const auto slots = catalog.slots();
    for (const format::Activity& row : catalog.activities()) {
        // Mod rows are numbered when the activity tables merge, so a pack names none.
        if (row.activityIndex != format::kAbsentIndex || catalog.string(row.internalName).empty()
            || row.scenarioIndex >= scenarios.size()
            || (row.flags & format::kActivityExactMask) != format::kActivityExactMask) {
            return false;
        }
    }
    for (const format::Bubble& row : bubbles) {
        if (row.scenarioIndex >= scenarios.size() || !range(row.states, states.size())) {
            return false;
        }
    }
    for (const format::State& row : states) {
        if (row.scenarioIndex >= scenarios.size() || row.bubbleIndex >= bubbles.size()
            || row.sliceSetIndex
                   != bubbles[row.bubbleIndex].bubbleOrdinal
                          * middleware::content::packages::tables::kSliceSetIndexFactor) {
            return false;
        }
    }
    for (const format::Occurrence& row : catalog.occurrences()) {
        if (row.scenarioIndex >= scenarios.size() || row.bubbleIndex >= bubbles.size()
            || row.stateIndex >= states.size() || row.objectIndex >= objects.size()) {
            return false;
        }
    }
    for (const format::Slot& row : slots) {
        if (row.objectIndex >= objects.size()) {
            return false;
        }
    }
    const auto scenes = catalog.authored_scene_resources();
    return std::all_of(scenes.begin(), scenes.end(), [&](const format::AuthoredSceneResource& row) {
        return row.slotIndex < slots.size();
    });
}

} // namespace

/** Mod rows are numbered when the tables merge, so a mod pack names no index. */
bool destination_matches(const Catalog& catalog,
                         const format::Activity& activityRow,
                         const activity::destination::DestinationSelection& destination) noexcept {
    const bool indexed = !catalog.from_mod();
    if (destination.activityIndex < 0
        || (indexed
            && static_cast<std::uint32_t>(destination.activityIndex)
                   != activityRow.activityIndex)) {
        return false;
    }
    const std::string_view liveName = package_name(destination);
    return !liveName.empty() && liveName == catalog.string(activityRow.internalName);
}

/** Checks the shared structure, then the rows only a mod pack has. */
bool valid_mod_catalog(const Catalog& value) noexcept {
    return validation::structure(value) && valid_rows(value);
}

/** Replaces the mod catalogs in one step. */
void publish_mod_catalogs(std::vector<Snapshot>&& catalogs) noexcept {
    try {
        g_mods.store(std::make_shared<const std::vector<Snapshot>>(std::move(catalogs)),
                     std::memory_order_release);
    } catch (...) {
        g_mods.store({}, std::memory_order_release);
    }
}

/** @return The mod catalogs, or null before any were published. */
std::shared_ptr<const std::vector<Snapshot>> mod_catalogs() noexcept {
    return g_mods.load(std::memory_order_acquire);
}

/** Reads the public flag from the pack's state rows; see runtime.h. */
bool mod_region_is_public(const BoundView& view,
                          std::int32_t effectiveRegion,
                          bool& isPublic) noexcept {
    isPublic = false;
    const format::Scenario* const scenario = bound_scenario(view);
    if (scenario == nullptr || effectiveRegion < 0) {
        return false;
    }
    const auto region = static_cast<std::uint32_t>(effectiveRegion);
    const format::State* owner = nullptr;
    for (const format::State& state : scenario_states(*view.catalog, *scenario)) {
        if (state.sliceSetIndex + state.stateOrdinal == region) {
            if (owner != nullptr) {
                return false;
            }
            owner = &state;
        }
    }
    if (owner == nullptr) {
        return false;
    }
    const auto states = bubble_states(*view.catalog, view.catalog->bubbles()[owner->bubbleIndex]);
    if (states.empty()) {
        return false;
    }
    isPublic = (states.front().publicValue & kPublicFlagMask) != 0;
    return true;
}

} // namespace sunrise::state::activity_sdk
