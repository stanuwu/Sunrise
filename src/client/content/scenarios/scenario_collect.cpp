#include <Windows.h>

#include <algorithm>
#include <string_view>

#include "../../../middleware/content/packages/named_tags.h"
#include "../../../middleware/content/packages/tables/bubble_state_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../spawn_sets/spawn_set_catalog_builder.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace packages = middleware::content::packages;

/** The name suffix every destination's client scenario carries. */
constexpr std::string_view kNameSuffix = ":scenario_client";

// A destination's stem is the key its spawn sets are looked up by, so the two storages must
// agree; a shorter one here would truncate the name and find nothing.
static_assert(layouts::kSpawnStemCapacity == state::build_data::spawn_sets::kStemNameCapacity);

/**
 * Records one live scenario tag and the map stem of the package it came from.
 * The stem is the key the destination's spawn sets are grouped under.
 * @param context Storage.
 * @param entry Live scenario tag with its package family.
 * @return False only when storage is full.
 */
bool collect_tag(void* context, const reader::ClassEntry& entry) noexcept {
    auto& storage = *static_cast<Storage*>(context);
    if (storage.liveTagCount >= storage.liveTags.size()) {
        return false;
    }
    LiveTag& live = storage.liveTags[storage.liveTagCount++];
    live.tag = entry.tag;
    // A package family with no representable stem leaves the row's stem empty, which costs the
    // destination its spawn-set list and nothing else.
    if (!spawn_sets::normalize_stem(entry.packageFamily, live.stem, live.stemLength)) {
        live.stem = {};
        live.stemLength = 0;
    }
    return true;
}

/**
 * Finds one row's tag in the class sweep.
 * @param storage Pass storage.
 * @return Its live-tag row, or null when the sweep does not carry it.
 */
[[nodiscard]] const LiveTag* live_tag(const Storage& storage, std::uint32_t tag) noexcept {
    for (std::size_t index = 0; index < storage.liveTagCount; ++index) {
        if (storage.liveTags[index].tag == tag) {
            return &storage.liveTags[index];
        }
    }
    return nullptr;
}

/**
 * Records one named scenario, keeping the newest patch of each destination.
 * A destination is named once per patch of its package and the tag differs between them. The
 * walk visits patch zero first, so keeping the first one seen would keep a stale tag.
 * @param context Storage.
 * @param entry Named definition from the package directory.
 * @return False only when row storage is full.
 */
bool collect_named(void* context, const packages::named_tags::Entry& entry) noexcept {
    auto& storage = *static_cast<Storage*>(context);
    const std::string_view name{entry.name.data(), entry.nameLength};
    if (entry.classId != packages::tables::kScenarioClass || !name.ends_with(kNameSuffix)) {
        return true;
    }
    const std::string_view destination = name.substr(0, name.size() - kNameSuffix.size());
    // A destination longer than the selection field could never be matched at lookup time.
    if (destination.empty() || destination.size() > layouts::kNameCapacity) {
        return true;
    }
    for (std::size_t row = 0; row < storage.rowCount; ++row) {
        layouts::Definition& existing = storage.rows[row];
        if (std::string_view(existing.name.data(), existing.nameLength) != destination) {
            continue;
        }
        if (entry.patchIndex >= storage.rowPatch[row]) {
            storage.rowPatch[row] = entry.patchIndex;
            existing.tag = entry.tag;
        }
        return true;
    }
    if (storage.rowCount >= storage.rows.size()) {
        return false;
    }
    layouts::Definition& definition = storage.rows[storage.rowCount];
    std::copy(destination.begin(), destination.end(), definition.name.begin());
    definition.nameLength = static_cast<std::uint8_t>(destination.size());
    definition.tag = entry.tag;
    storage.rowPatch[storage.rowCount] = entry.patchIndex;
    ++storage.rowCount;
    return true;
}

/**
 * Adds one package id a destination loads.
 * @param definition Destination row.
 */
void add_package(layouts::Definition& definition, std::uint16_t packageId) noexcept {
    if (packageId == packages::tables::kAbsentPackageId
        || definition.packageCount >= definition.packages.size()) {
        return;
    }
    for (std::size_t index = 0; index < definition.packageCount; ++index) {
        if (definition.packages[index] == packageId) {
            return;
        }
    }
    definition.packages[definition.packageCount++] = packageId;
}

/**
 * Reads one row's bubble layout.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage owning the blob buffer.
 * @param definition Row to fill.
 * @return True when the blob read and parsed as a scenario.
 */
[[nodiscard]] bool resolve_row(const reader::Source& source,
                               reader::Scratch& scratch,
                               Storage& storage,
                               layouts::Definition& definition) noexcept {
    packages::tables::BubbleStates states{};
    if (!reader::read_tag(source, scratch, definition.tag, storage.blob)
        || !packages::tables::bubble_states(storage.blob, states)) {
        return false;
    }
    definition.bubbleCount = static_cast<std::uint8_t>(states.count);
    definition.truncated = states.truncated ? 1U : 0U;
    std::copy(states.bytes.begin(), states.bytes.end(), definition.bubbleStates.begin());
    std::copy(states.hashes.begin(), states.hashes.end(), definition.bubbleHashes.begin());
    std::copy(
        states.stateCounts.begin(), states.stateCounts.end(), definition.bubbleStateCounts.begin());
    std::copy(
        states.mapIndices.begin(), states.mapIndices.end(), definition.bubbleMapIndices.begin());
    definition.packageCount = 0;
    // The destination's own package is loaded too, and a scenario whose bubbles name none would
    // otherwise claim it loads nothing at all.
    add_package(definition, packages::tables::package_of(definition.tag));
    for (std::size_t index = 0; index < states.packageCount; ++index) {
        add_package(definition, states.packages[index]);
    }
    return true;
}

} // namespace

/**
 * Sweeps the installed packages for scenario tags and matches them to destination names.
 * @param source Package directory and borrowed block keys.
 * @param storage Pass storage receiving the live tags and the named rows.
 * @param reason Receives the step that refused, or stays null.
 * @return True when both steps finished.
 */
bool collect_rows(const reader::Source& source, Storage& storage, const char*& reason) noexcept {
    reason = nullptr;
    storage.liveTagCount = 0;
    storage.rowCount = 0;
    storage.rowPatch.fill(0);
    packages::reader::ScanResult scan{};
    if (!packages::reader::scan_class_entries(
            source.directory, packages::tables::kScenarioClass, &collect_tag, &storage, scan)) {
        reason = "sweep";
        return false;
    }
    packages::named_tags::DirectoryResult names{};
    if (!packages::named_tags::extract_directory(
            source.directory, &collect_named, &storage, names)) {
        reason = "names";
        return false;
    }
    storage.resolved.fill(0);
    storage.resolvedCount = 0;
    storage.resolveCursor = 0;
    storage.liveRowCount = 0;
    for (std::size_t row = 0; row < storage.rowCount; ++row) {
        const LiveTag* const live = live_tag(storage, storage.rows[row].tag);
        if (live == nullptr) {
            continue;
        }
        ++storage.liveRowCount;
        storage.rows[row].spawnStem = live->stem;
        storage.rows[row].spawnStemLength = live->stemLength;
    }
    storage.resolveDeadlineTick = GetTickCount64() + kResolveWindowMs;
    return true;
}

/**
 * Reads the bubble layout of rows that have not read yet, within this call's budget.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage carrying the resolve cursor.
 * @return True when the collection has settled and may be compacted.
 */
bool resolve_pending(const reader::Source& source,
                     reader::Scratch& scratch,
                     Storage& storage) noexcept {
    std::size_t reads = 0;
    std::size_t visited = 0;
    while (visited < storage.rowCount && reads < kResolveReadBudget) {
        const std::size_t row = storage.resolveCursor;
        storage.resolveCursor = (storage.resolveCursor + 1) % storage.rowCount;
        ++visited;
        if (storage.resolved[row] != 0) {
            continue;
        }
        ++reads;
        if (resolve_row(source, scratch, storage, storage.rows[row])) {
            storage.resolved[row] = 1;
            ++storage.resolvedCount;
        }
    }
    // Every row the sweep still carries has read, or the window has closed on the rest.
    return storage.resolvedCount >= storage.liveRowCount
           || GetTickCount64() >= storage.resolveDeadlineTick;
}

/**
 * Re-arms the resolve window so a pass that read nothing tries the whole row set again.
 * The rows and the name match are kept: only the per-row read outcome is cleared, because every
 * read failed for one reason, the block keys were not ready yet.
 * @param storage Pass storage whose resolve state is cleared.
 */
void rearm_resolve(Storage& storage) noexcept {
    storage.resolved.fill(0);
    storage.resolvedCount = 0;
    storage.resolveCursor = 0;
    storage.resolveDeadlineTick = GetTickCount64() + kResolveWindowMs;
    ++storage.resolveRounds;
}

/**
 * Moves every resolved row to the front of the row array.
 * @param storage Pass storage whose kept count is set here.
 */
void compact_rows(Storage& storage) noexcept {
    std::size_t kept = 0;
    for (std::size_t row = 0; row < storage.rowCount; ++row) {
        if (storage.resolved[row] != 0) {
            storage.rows[kept++] = storage.rows[row];
        }
    }
    storage.keptCount = kept;
    storage.blob.clear();
    storage.blob.shrink_to_fit();
}

} // namespace sunrise::client::content::scenarios
