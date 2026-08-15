#include "ability_bucket_catalog.h"

#include "../table.h"

namespace sunrise::state::build_data::abilities {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;

/** @return True when both rows name the same subclass selection. */
[[nodiscard]] bool same_key(const Definition& left, const Definition& right) noexcept {
    return left.socketEntryListIndex == right.socketEntryListIndex
           && left.selection == right.selection;
}

} // namespace

/** Clears every generated ability bucket row under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

/** Checks that no two rows share a subclass and ability selection. */
bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        for (std::size_t other = row + 1; other < definitions.size(); ++other) {
            if (same_key(definitions[row], definitions[other])) {
                return false;
            }
        }
    }
    return true;
}

/** Replaces the generated ability bucket rows in one step. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_definitions.replace(definitions);
}

/** Finds the buckets one subclass publishes under one ability selection. */
bool find(std::uint16_t socketEntryListIndex,
          const Selection& selection,
          Definition& definition) noexcept {
    definition = {};
    const Definition wanted{socketEntryListIndex, selection};
    const Lock::Shared guard(g_lock);
    for (const Definition& row : g_definitions.rows()) {
        if (same_key(row, wanted)) {
            definition = row;
            return true;
        }
    }
    return false;
}

/** Copies every row in publication order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return Number of generated ability bucket rows, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::abilities
