#include "combat_catalog.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <tuple>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::combat {
namespace {

/** An empty name hashes to the FNV-1 basis, which names no class. */
constexpr std::uint32_t kEmptyNameHash = 0x811C9DC5U;

core::threading::SrwLock g_lock;
Table<EnemyRace, kEnemyRaceCapacity> g_races;
Table<LabelGroup, kLabelGroupCapacity> g_groups;
bool g_unsupported{};

/** @return True when a name hash can name an installed class, label or group. */
[[nodiscard]] bool named(std::uint32_t hash) noexcept {
    return hash != 0 && hash != kEmptyNameHash;
}

} // namespace

/** Clears both tables and forgets an unsupported settlement. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_races.clear();
    g_groups.clear();
    g_unsupported = false;
}

/** @return True once the installed data has settled that no taxonomy can be read. */
bool unsupported() noexcept {
    const std::shared_lock guard(g_lock);
    return g_unsupported;
}

/** Discards both tables and records that the installed data supports no taxonomy. */
void settle_unsupported() noexcept {
    const std::lock_guard guard(g_lock);
    g_races.clear();
    g_groups.clear();
    g_unsupported = true;
}

/** Ascending class order lets a lookup binary-search the table. */
bool valid(std::span<const EnemyRace> races) noexcept {
    if (races.empty() || races.size() > kEnemyRaceCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < races.size(); ++index) {
        if (!named(races[index].classHash)
            || (index != 0 && races[index].classHash <= races[index - 1].classHash)) {
            return false;
        }
    }
    return true;
}

/** Ascending group-then-label order lets a membership test binary-search the table. */
bool valid(std::span<const LabelGroup> groups) noexcept {
    if (groups.empty() || groups.size() > kLabelGroupCapacity) {
        return false;
    }
    const auto key = [](const LabelGroup& row) noexcept {
        return std::tie(row.groupHash, row.labelHash);
    };
    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (!named(groups[index].groupHash) || !named(groups[index].labelHash)
            || (index != 0 && key(groups[index]) <= key(groups[index - 1]))) {
            return false;
        }
    }
    return true;
}

/** Replaces both tables together, so a reader never sees one without the other. */
bool replace(std::span<const EnemyRace> races, std::span<const LabelGroup> groups) noexcept {
    if (!valid(races) || !valid(groups)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    if (!g_races.replace(races) || !g_groups.replace(groups)) {
        g_races.clear();
        g_groups.clear();
        return false;
    }
    g_unsupported = false;
    return true;
}

/** Finds one class by binary search over the ascending class order. */
bool find_race(std::uint32_t classHash, std::uint8_t& race) noexcept {
    race = 0;
    const std::shared_lock guard(g_lock);
    const auto rows = g_races.rows();
    const auto at = std::lower_bound(
        rows.begin(), rows.end(), classHash, [](const EnemyRace& row, std::uint32_t key) {
            return row.classHash < key;
        });
    if (at == rows.end() || at->classHash != classHash) {
        return false;
    }
    race = at->race;
    return true;
}

/** Tests one membership by binary search over the ascending group-then-label order. */
bool in_group(std::uint32_t groupHash, std::uint32_t labelHash) noexcept {
    const std::shared_lock guard(g_lock);
    const auto rows = g_groups.rows();
    const LabelGroup wanted{groupHash, labelHash};
    return std::binary_search(
        rows.begin(), rows.end(), wanted, [](const LabelGroup& left, const LabelGroup& right) {
            return std::tie(left.groupHash, left.labelHash)
                   < std::tie(right.groupHash, right.labelHash);
        });
}

/** Copies every race row in class order. */
bool snapshot(std::span<EnemyRace> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_races.snapshot(output, count);
}

/** Copies every group row in group-then-label order. */
bool snapshot(std::span<LabelGroup> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_groups.snapshot(output, count);
}

/** @return The race row count, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_races.count();
}

} // namespace sunrise::state::build_data::combat
