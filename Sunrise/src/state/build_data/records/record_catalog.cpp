#include "record_catalog.h"

#include "../table.h"

namespace sunrise::state::build_data::records {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;

} // namespace

/** Clears every generated record definition under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

/** Checks that the definitions are dense and in native index order. */
bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        if (definitions[row].definitionIndex != row) {
            return false;
        }
    }
    return true;
}

/** Replaces the generated record definitions in one step. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_definitions.replace(definitions);
}

/** Finds one record by the native row a claim names. */
bool find(std::uint16_t definitionIndex, Definition& definition) noexcept {
    const Lock::Shared guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    if (definitionIndex >= rows.size()) {
        return false;
    }
    definition = rows[definitionIndex];
    return true;
}

/** Copies every row in native record order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return Number of generated record definitions, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::records
