#include "scenario_build.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/runtime/build_data_invalidation.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace packages = middleware::content::packages;

/**
 * Reports the pass so a boot that falls back to authored defaults says which step lost the rows.
 * Each count is separate on purpose: a sweep that finds tags, a name match that finds none, and a
 * blob read that drops every row all end with no domain and need different fixes.
 * @param storage Pass storage holding every count.
 * @param kept Rows whose blob read and parsed.
 * @param rostered Rows that published at least one roster group.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage,
            std::size_t kept,
            std::size_t rostered,
            const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=build_data stage=scenarios tags=%zu named=%zu live=%zu kept=%zu "
                      "groups=%zu dropped=%zu rostered=%zu result=%s",
                      storage.liveTagCount,
                      storage.rowCount,
                      storage.liveRowCount,
                      kept,
                      storage.roster.groupCount,
                      storage.roster.unresolvedGroups,
                      rostered,
                      result);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         kept != 0 ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Walks the next batch of rosters and publishes the domain once the walk finishes.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage holding the collected rows and the walk cursor.
 * @return True only when the whole domain is published.
 */
[[nodiscard]] bool walk_rosters(const packages::reader::Source& source,
                                packages::reader::Scratch& scratch,
                                Storage& storage) noexcept {
    const auto rows = std::span(storage.rows).first(storage.keptCount);
    if (!build_rosters(source, scratch, storage.roster, rows)) {
        return false;
    }
    std::size_t rostered = 0;
    for (const layouts::Definition& row : rows) {
        rostered += row.rosterGroupCount != 0 ? 1U : 0U;
    }
    const bool published = state::build_data::publish_scenario_layouts(
        rows, std::span(storage.roster.groups).first(storage.roster.groupCount));
    report(storage, storage.keptCount, rostered, published ? "ok" : "publish");
    storage.roster = {};
    return published;
}

/**
 * Collects the rows and reads their bubble layouts, over as many calls as the reads need.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage carrying the collection state between calls.
 * @return True once the rows are compacted and the roster walk may start.
 */
[[nodiscard]] bool collect_layouts(const packages::reader::Source& source,
                                   packages::reader::Scratch& scratch,
                                   Storage& storage) noexcept {
    if (!storage.collected) {
        const char* reason = nullptr;
        if (!collect_rows(source, storage, reason)) {
            report(storage, 0, 0, reason);
            return false;
        }
        storage.collected = true;
        report(storage, 0, 0, "collecting");
        return false;
    }
    if (!storage.compacted) {
        if (!resolve_pending(source, scratch, storage)) {
            if (progress_report_due(storage, GetTickCount64())) {
                report(storage, storage.resolvedCount, 0, "waiting");
            }
            return false;
        }
        compact_rows(storage);
        // An empty result is never a finished pass: a read before the block keys arrive returns
        // nothing. Report only the first empty round, since the retry runs on every worker slice.
        if (storage.keptCount == 0) {
            if (storage.resolveRounds == 0) {
                report(storage, 0, 0, "empty");
            }
            rearm_resolve(storage);
            return false;
        }
        report(storage, storage.keptCount, 0, "collected");
        storage.compacted = true;
    }
    return true;
}

} // namespace

/** Extracts every destination's bubble layout and roster from the installed packages, once. */
bool build(const packages::reader::Source& source, packages::reader::Scratch& scratch) noexcept {
    if (state::build_data::scenario_layouts_ready()) {
        return true;
    }
    static Storage storage{};
    // A live reload dropped the layouts; the pass starts over. In place, as the storage is too big
    // for the stack.
    static std::uint64_t builtRevision{};
    if (const auto revision = state::build_data::invalidation::revision(
            state::build_data::invalidation::kScenarioLayouts);
        revision != builtRevision) {
        builtRevision = revision;
        std::destroy_at(&storage);
        std::construct_at(&storage);
    }
    return collect_layouts(source, scratch, storage) && walk_rosters(source, scratch, storage);
}

/** Collects every row and reads each bubble layout once, without rosters or a publish. */
bool LayoutSet::build(const packages::reader::Source& source,
                      packages::reader::Scratch& scratch) noexcept {
    rows_.clear();
    try {
        // The pass storage is large and only the kept rows outlive it.
        const auto storage = std::make_unique<Storage>();
        const char* reason = nullptr;
        if (!collect_rows(source, *storage, reason)) {
            return false;
        }
        resolve_all(source, scratch, *storage);
        compact_rows(*storage);
        rows_.assign(storage->rows.begin(),
                     storage->rows.begin() + static_cast<std::ptrdiff_t>(storage->keptCount));
    } catch (...) {
        rows_.clear();
        return false;
    }
    return !rows_.empty();
}

/** Finds one destination's layout among the kept rows. */
bool LayoutSet::find(std::string_view name, layouts::Definition& definition) const noexcept {
    definition = {};
    if (name.empty()) {
        return false;
    }
    for (const layouts::Definition& row : rows_) {
        if (std::string_view(row.name.data(), row.nameLength) == name) {
            definition = row;
            return true;
        }
    }
    return false;
}

/** @return The rows the last build kept. */
std::span<const layouts::Definition> LayoutSet::rows() const noexcept {
    return rows_;
}

/** Finds one destination's layout in a caller's set, or in State when the set is null. */
bool find_layout(const LayoutSet* set,
                 std::string_view name,
                 layouts::Definition& definition) noexcept {
    return set != nullptr ? set->find(name, definition)
                          : state::build_data::find_scenario_layout(name, definition);
}

} // namespace sunrise::client::content::scenarios
