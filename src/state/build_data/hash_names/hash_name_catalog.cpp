#include "hash_name_catalog.h"

#include <algorithm>

#include "../table.h"

namespace sunrise::state::build_data::hash_names {
namespace {

Lock g_lock;
Table<Name, kNameCapacity> g_names;

/** @return True when the name is a valid identifier and fits its storage. */
[[nodiscard]] bool canonical(const Name& row) noexcept {
    if (row.nameLength == 0 || row.nameLength > kNameLength) {
        return false;
    }
    for (std::size_t index = 0; index < kNameLength; ++index) {
        const bool declared = index < row.nameLength;
        // Storage past the name must stay zero, or two caches of the same packages could differ
        // byte for byte while meaning the same thing.
        if (declared ? !name_character(row.name[index]) : row.name[index] != '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

/** Clears every resolved bubble name under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_names.clear();
}

/** Checks one complete table in ascending hash order. */
bool valid(std::span<const Name> names) noexcept {
    if (names.size() > kNameCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < names.size(); ++row) {
        // A repeated or unordered hash would make the lookup depend on row order.
        if (!canonical(names[row]) || (row != 0 && names[row - 1].hash >= names[row].hash)) {
            return false;
        }
    }
    return true;
}

/** Replaces the resolved bubble names in one step. */
bool replace(std::span<const Name> names) noexcept {
    if (!valid(names)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_names.replace(names);
}

/** Finds one bubble name by its hash. */
bool find(std::uint32_t hash, Name& name) noexcept {
    name = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Name> rows = g_names.rows();
    const auto found =
        std::lower_bound(rows.begin(), rows.end(), hash, [](const Name& row, std::uint32_t key) {
            return row.hash < key;
        });
    const bool present = found != rows.end() && found->hash == hash;
    if (present) {
        name = *found;
    }
    return present;
}

/** Copies every row in ascending hash order. */
bool snapshot(std::span<Name> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_names.snapshot(output, count);
}

/** @return Number of resolved names, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_names.count();
}

} // namespace sunrise::state::build_data::hash_names
