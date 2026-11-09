#include "pursuit_catalog.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::pursuits {
namespace {

core::threading::SrwLock g_lock;
Table<Definition, kDefinitionCapacity> g_contracts;
bool g_ready{};
bool g_unsupported{};
std::uint64_t g_revision{};

/** Declared objectives name rows of the table, and the slots after them stay zero. */
[[nodiscard]] bool valid_objectives(const Definition& row, std::size_t objectiveCount) noexcept {
    if (row.objectiveCount > row.objectives.size()) {
        return false;
    }
    const auto declared = std::span(row.objectives).first(row.objectiveCount);
    const auto unused = std::span(row.objectives).subspan(row.objectiveCount);
    return std::all_of(declared.begin(),
                       declared.end(),
                       [objectiveCount](std::uint16_t index) { return index < objectiveCount; })
           && std::all_of(
               unused.begin(), unused.end(), [](std::uint16_t index) { return index == 0; });
}

/** Checks one contract's own fields. The caller checks its item identity. */
[[nodiscard]] bool valid_contract(const Definition& row, std::size_t objectiveCount) noexcept {
    return row.definitionIndex != kAbsent && row.lifetimeSeconds >= 0
           && valid_objectives(row, objectiveCount);
}

} // namespace

/** Discards every contract and forgets an unsupported settlement. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_contracts.clear();
    g_ready = false;
    g_unsupported = false;
    ++g_revision;
}

/** @return True once the contracts have been published, even when there are none. */
bool ready() noexcept {
    const std::shared_lock guard(g_lock);
    return g_ready;
}

/** @return True once the installed data has settled that no contract can be read. */
bool unsupported() noexcept {
    const std::shared_lock guard(g_lock);
    return g_unsupported;
}

/** Discards every contract and records that the installed data supports none. */
void settle_unsupported() noexcept {
    const std::lock_guard guard(g_lock);
    g_contracts.clear();
    g_ready = false;
    g_unsupported = true;
    ++g_revision;
}

/** @return A generation that changes after every publication, settlement or clear. */
std::uint64_t revision() noexcept {
    const std::shared_lock guard(g_lock);
    return g_revision;
}

/** Checks contract order and objective references. */
bool valid(std::span<const Definition> rows, std::size_t objectiveCount) noexcept {
    if (rows.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (!valid_contract(rows[index], objectiveCount)
            || (index != 0 && rows[index - 1].definitionIndex >= rows[index].definitionIndex)) {
            return false;
        }
    }
    return true;
}

/** Publishes validated contracts under the catalog lock. */
bool replace(std::span<const Definition> rows, std::size_t objectiveCount) noexcept {
    if (!valid(rows, objectiveCount)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    if (!g_contracts.replace(rows)) {
        return false;
    }
    g_ready = true;
    g_unsupported = false;
    ++g_revision;
    return true;
}

/** Contracts are stored in ascending item order, so the lookup is a binary search. */
bool find(std::uint16_t itemIndex, Definition& output) noexcept {
    output = {};
    const std::shared_lock guard(g_lock);
    const auto contracts = g_contracts.rows();
    const auto found = std::lower_bound(
        contracts.begin(),
        contracts.end(),
        itemIndex,
        [](const Definition& row, std::uint16_t key) { return row.definitionIndex < key; });
    if (!g_ready || found == contracts.end() || found->definitionIndex != itemIndex) {
        return false;
    }
    output = *found;
    return true;
}

/** Copies every contract; count is zero when the output is too small. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_contracts.snapshot(output, count);
}

} // namespace sunrise::state::build_data::pursuits
