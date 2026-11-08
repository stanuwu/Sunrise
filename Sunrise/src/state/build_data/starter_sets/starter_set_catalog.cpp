#include "starter_set_catalog.h"

#include <mutex>
#include <shared_mutex>

#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::starter_sets {
namespace {

core::threading::SrwLock g_lock;
/** Indexed by class byte. */
std::array<Definition, kClassCount> g_rows{};
std::array<bool, kClassCount> g_present{};
bool g_published{};

} // namespace

/** Clears the published rows under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_rows = {};
    g_present = {};
    g_published = false;
}

/** A class may appear once; its item count must fit the row. */
bool valid(std::span<const Definition> definitions) noexcept {
    std::array<bool, kClassCount> present{};
    for (const Definition& definition : definitions) {
        if (definition.characterClass >= kClassCount || present[definition.characterClass]
            || definition.itemCount > kItemCapacity) {
            return false;
        }
        present[definition.characterClass] = true;
    }
    return true;
}

/** Rows are placed by class before the lock is taken, so readers see one whole table. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    std::array<Definition, kClassCount> rows{};
    std::array<bool, kClassCount> present{};
    for (const Definition& definition : definitions) {
        rows[definition.characterClass] = definition;
        present[definition.characterClass] = true;
    }
    const std::lock_guard guard(g_lock);
    g_rows = rows;
    g_present = present;
    g_published = true;
    return true;
}

/** @return True once a table, empty or not, is published. */
bool ready() noexcept {
    const std::shared_lock guard(g_lock);
    return g_published;
}

/** Finds the row of one class under the catalog lock. */
bool find(std::uint8_t characterClass, Definition& definition) noexcept {
    definition = {};
    const std::shared_lock guard(g_lock);
    if (characterClass >= kClassCount || !g_present[characterClass]) {
        return false;
    }
    definition = g_rows[characterClass];
    return true;
}

/** Copies under the catalog lock, so the rows come from one published table. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    count = 0;
    const std::shared_lock guard(g_lock);
    std::size_t used = 0;
    for (std::size_t characterClass = 0; characterClass < kClassCount; ++characterClass) {
        if (!g_present[characterClass]) {
            continue;
        }
        if (used >= output.size()) {
            return false;
        }
        output[used++] = g_rows[characterClass];
    }
    count = used;
    return true;
}

} // namespace sunrise::state::build_data::starter_sets
