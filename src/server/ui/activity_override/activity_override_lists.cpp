#include "activity_override_lists.h"

#include <algorithm>
#include <cstdio>
#include <span>

#include "../../../middleware/content/packages/tables/component_container_reader.h"
#include "../../../middleware/content/packages/tables/region_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/spawn_reader.h"
#include "../../../state/activity/forced/definition.h"
#include "../../../state/build_data/runtime.h"

namespace sunrise::server::ui::activity_override {
namespace {

namespace layouts = state::build_data::scenarios;
namespace tables = middleware::content::packages::tables;

/** A bubble's slice sets are spaced by this factor, so it also caps how many it can declare. */
constexpr std::uint8_t kMaximumBubbleStates = tables::kSliceSetIndexFactor;

Lists g_lists{};

/** @return The destination row's name as a bounded view. */
[[nodiscard]] std::string_view name_of(const layouts::Definition& row) noexcept {
    return {row.name.data(), row.nameLength};
}

/** @return The destination row's map stem as a bounded view. */
[[nodiscard]] std::string_view stem_of(const layouts::Definition& row) noexcept {
    return {row.spawnStem.data(), row.spawnStemLength};
}

/** @return True when the left destination row sorts before the right one by name. */
[[nodiscard]] bool name_less(const layouts::Definition& left,
                             const layouts::Definition& right) noexcept {
    return name_of(left) < name_of(right);
}

/**
 * Names one spawn set.
 * The packages carry no name table for these. Only the two hashes the client itself defines are
 * named, and every other set is shown by hash.
 * @return The name, or an empty view.
 */
[[nodiscard]] std::string_view spawn_name(std::uint32_t hash) noexcept {
    if (hash == tables::kDefaultSpawnNameHash) {
        return "default";
    }
    return hash == tables::kUnnamedSpawnNameHash ? std::string_view("unnamed") : std::string_view();
}

/**
 * Finds the internal name the packages give one hash.
 * @param hash Bubble or spawn-set name hash.
 * @param storage Gets the found row.
 * @return The name, or an empty view where no package names the hash.
 */
[[nodiscard]] std::string_view resolve_name(std::uint32_t hash,
                                            state::build_data::hash_names::Name& storage) noexcept {
    if (!state::build_data::find_hash_name(hash, storage)) {
        return {};
    }
    return {storage.name.data(), storage.nameLength};
}

/**
 * Writes the name column of one row, which stays empty when there is no name.
 * The separator sits inside the column, so an unnamed row ends after its hash with no gap.
 * @param name The found name, which may be empty.
 * @param column Gets the separator and the name.
 */
void name_column(std::string_view name, Label& column) noexcept {
    column = {};
    if (name.empty()) {
        return;
    }
    (void)std::snprintf(
        column.data(), column.size(), "  %.*s", static_cast<int>(name.size()), name.data());
}

/** Copies text into one label. @param label Gets the text and a null. */
void assign(std::string_view text, Label& label) noexcept {
    label = {};
    const std::size_t length = (std::min)(text.size(), label.size() - 1);
    std::copy_n(text.begin(), length, label.begin());
}

/** Clears the slice-set rows, which belong to one bubble rather than to the destination. */
void clear_slices(Lists& rows) noexcept {
    rows.slices = {};
    rows.sliceValues = {};
    rows.sliceCount = 0;
}

/** Clears the spawn-set rows, which are rebuilt per destination and again per bubble. */
void clear_spawns(Lists& rows) noexcept {
    rows.spawns = {};
    rows.spawnHashes = {};
    rows.spawnCount = 0;
    rows.spawnUnavailable = false;
    rows.spawnNarrowed = false;
    rows.spawnHidden = 0;
    rows.spawnForeign = 0;
}

/** Clears every list that belongs to the selected destination. */
void clear_destination(Lists& rows) noexcept {
    rows.selected = {};
    rows.bubbles = {};
    rows.bubbleOrdinals = {};
    rows.bubbleCount = 0;
    clear_slices(rows);
    clear_spawns(rows);
}

/** Builds the bubble rows of one destination. @param layout Published destination layout. */
void build_bubbles(Lists& rows, const layouts::Definition& layout) noexcept {
    const std::size_t declared =
        (std::min)(static_cast<std::size_t>(layout.bubbleCount), layout.bubbleStates.size());
    for (std::size_t bubble = 0; bubble < declared; ++bubble) {
        const std::uint8_t states =
            (std::min)(layout.bubbleStateCounts[bubble], kMaximumBubbleStates);
        // The hash always, then the name where the packages produce one. Around four in ten
        // bubbles are cinematic or orbit spaces that carry no name at all.
        state::build_data::hash_names::Name storage{};
        Label name{};
        name_column(resolve_name(layout.bubbleHashes[bubble], storage), name);
        Label label{};
        const int written = std::snprintf(label.data(),
                                          label.size(),
                                          "%2zu  0x%08X%s  (%u slice%s)",
                                          bubble,
                                          layout.bubbleHashes[bubble],
                                          name.data(),
                                          static_cast<unsigned>(states),
                                          states == 1 ? "" : "s");
        if (written <= 0) {
            continue;
        }
        rows.bubbles[rows.bubbleCount] = label;
        rows.bubbleOrdinals[rows.bubbleCount] = static_cast<std::uint8_t>(bubble);
        ++rows.bubbleCount;
    }
}

/**
 * Tells whether this destination loads a package that declares one set.
 * A map package always loads. An activity package loads only for the destinations whose own
 * slice-set entries name it. A set in any other one never attaches.
 * @param layout Selected destination.
 * @return True when some package declaring the set is one this destination loads.
 */
[[nodiscard]] bool loads_package(const layouts::Definition& layout,
                                 const state::build_data::spawn_sets::NameHash& row) noexcept {
    if (row.inMapPackage != 0) {
        return true;
    }
    const std::size_t declared =
        (std::min)(static_cast<std::size_t>(layout.packageCount), layout.packages.size());
    for (std::size_t index = 0; index < row.activityPackageCount; ++index) {
        for (std::size_t package = 0; package < declared; ++package) {
            if (layout.packages[package] == row.activityPackages[index]) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Builds the spawn-set rows of one destination, optionally narrowed to one bubble.
 * A set names the bubbles it is offered in. A set that reached no container names none, and is
 * offered everywhere on the map as a candidate.
 * @param stem Map stem the destination loads from.
 * @param mapIndex Map-global bubble index, or the absent index for the whole map.
 */
void build_spawns(Lists& rows, std::string_view stem, std::uint16_t mapIndex) noexcept {
    if (stem.empty()) {
        return;
    }
    static std::array<state::build_data::spawn_sets::NameHash, kSpawnCapacity> scratch{};
    std::size_t count = 0;
    if (!state::build_data::find_spawn_sets(stem, scratch, count)) {
        rows.spawnUnavailable = true;
        return;
    }
    const bool narrowing = mapIndex != tables::kAbsentMapBubbleIndex;
    rows.spawnNarrowed = narrowing;
    for (std::size_t index = 0; index < count && rows.spawnCount < rows.spawns.size(); ++index) {
        const state::build_data::spawn_sets::NameHash& row = scratch[index];
        const std::uint32_t hash = row.value;
        const bool offered = !narrowing || tables::bubble_in_mask(row.bubbleMask, mapIndex);
        if (!offered && row.unbound == 0) {
            ++rows.spawnHidden;
            continue;
        }
        // The hash always. Its name is one of the two the client itself defines, else the
        // extracted one, else nothing.
        std::string_view named = spawn_name(hash);
        state::build_data::hash_names::Name storage{};
        if (named.empty()) {
            named = resolve_name(hash, storage);
        }
        Label name{};
        name_column(named, name);
        // A set reaching this bubble only because nothing binds it is marked, not hidden.
        const char* const candidate = offered ? "" : "  (candidate)";
        // A set this destination does not load is marked too. The bubble is right and the set
        // still will not attach, which is what lands the player nowhere.
        const bool loaded = loads_package(rows.selected, row);
        if (!loaded) {
            ++rows.spawnForeign;
        }
        Label label{};
        const int written = std::snprintf(label.data(),
                                          label.size(),
                                          "0x%08X%s%s%s",
                                          hash,
                                          name.data(),
                                          candidate,
                                          loaded ? "" : "  (not loaded)");
        if (written <= 0) {
            continue;
        }
        rows.spawns[rows.spawnCount] = label;
        rows.spawnHashes[rows.spawnCount] = hash;
        ++rows.spawnCount;
    }
}

} // namespace

/** @return Process-lifetime picker rows, which no other module reads. */
Lists& lists() noexcept {
    return g_lists;
}

/** Rebuilds the destination rows when the published layout count has changed. */
void refresh_activities(Lists& rows) noexcept {
    const std::size_t published = state::build_data::scenario_layout_count();
    if (published == rows.activityRevision) {
        return;
    }
    rows.activityRevision = published;
    rows.activities = {};
    rows.activityCount = 0;
    // The snapshot is a whole domain of fixed rows, so it is static rather than a local.
    static std::array<layouts::Definition, layouts::kDefinitionCapacity> scratch{};
    std::size_t count = 0;
    if (!state::build_data::snapshot_scenario_layouts(scratch, count)) {
        return;
    }
    const auto rowsRead = std::span(scratch).first(count);
    // Extraction order is package order, which no reader can search. Name order is.
    std::sort(rowsRead.begin(), rowsRead.end(), name_less);
    for (const layouts::Definition& row : rowsRead) {
        assign(name_of(row), rows.activities[rows.activityCount]);
        ++rows.activityCount;
    }
}

/** Rebuilds the bubble and spawn-set rows for one destination, and clears the slice-set rows. */
void refresh_destination(Lists& rows, std::string_view name) noexcept {
    clear_destination(rows);
    layouts::Definition layout{};
    if (name.empty() || !state::build_data::find_scenario_layout(name, layout)) {
        return;
    }
    rows.selected = layout;
    build_bubbles(rows, layout);
    // The whole map until a bubble is picked, because the list is what a bubble narrows.
    build_spawns(rows, stem_of(layout), tables::kAbsentMapBubbleIndex);
}

/** Rebuilds the slice-set and spawn-set rows for one bubble of the selected destination. */
void refresh_bubble(Lists& rows, std::uint8_t bubble) noexcept {
    clear_slices(rows);
    clear_spawns(rows);
    if (bubble >= rows.selected.bubbleCount || bubble >= rows.selected.bubbleStates.size()) {
        build_spawns(rows, stem_of(rows.selected), tables::kAbsentMapBubbleIndex);
        return;
    }
    build_spawns(rows, stem_of(rows.selected), rows.selected.bubbleMapIndices[bubble]);
    const std::uint8_t states =
        (std::min)(rows.selected.bubbleStateCounts[bubble], kMaximumBubbleStates);
    const std::uint32_t base = tables::region_index(bubble);
    for (std::uint8_t state = 0; state < states && rows.sliceCount < rows.slices.size(); ++state) {
        const std::uint32_t value = base + state;
        Label label{};
        const int written = std::snprintf(
            label.data(), label.size(), "%u  state %u", value, static_cast<unsigned>(state));
        if (written <= 0) {
            continue;
        }
        rows.slices[rows.sliceCount] = label;
        rows.sliceValues[rows.sliceCount] = static_cast<std::uint16_t>(value);
        ++rows.sliceCount;
    }
}

} // namespace sunrise::server::ui::activity_override
