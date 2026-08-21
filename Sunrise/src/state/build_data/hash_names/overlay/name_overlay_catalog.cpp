#include "name_overlay_catalog.h"

#include <algorithm>

#include "../../table.h"
#include "../hash_name_catalog.h"

namespace sunrise::state::build_data::hash_names::overlay {
namespace {

Lock g_lock;
Table<Name, kNameCapacity> g_names;

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_names.clear();
}

bool replace(std::span<const Name> names) noexcept {
    if (!hash_names::valid(names)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_names.replace(names);
}

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

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_names.count();
}

} // namespace sunrise::state::build_data::hash_names::overlay
