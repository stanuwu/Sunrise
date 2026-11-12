#include "insertion_rule_catalog.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include "../../../unlocks/definition.h"
#include "../../../unlocks/unlocks_expression.h"
#include "../../table.h"
#include "../item_catalog.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::items::insertion_rules {
namespace {
core::threading::SrwLock g_lock;
Table<Rule, kRuleCapacity> g_rules;
bool g_unsupported{};
} // namespace

/** Clears the rule rows and forgets an unsupported settlement. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_rules.clear();
    g_unsupported = false;
}

/** @return True once the installed data has settled that no rule list can be published. */
bool unsupported() noexcept {
    const std::shared_lock guard(g_lock);
    return g_unsupported;
}

/** Discards every rule row and records that the installed data supports none. */
void settle_unsupported() noexcept {
    const std::lock_guard guard(g_lock);
    g_rules.clear();
    g_unsupported = true;
}

/** Checks one complete rule list; an unreadable row carries nothing but its plug. */
bool valid(std::span<const Rule> rules) noexcept {
    if (rules.empty() || rules.size() > kRuleCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const Rule& rule = rules[index];
        if ((index != 0 && rule.plugDefinitionIndex <= rules[index - 1].plugDefinitionIndex)
            || rule.plugDefinitionIndex >= kDefinitionCapacity) {
            return false;
        }
        if (!rule.readable) {
            if (rule.conditionCount != 0 || rule.requiredFlagIndex != kUnavailableFlagIndex) {
                return false;
            }
            continue;
        }
        if (rule.conditionCount == 0 || rule.conditionCount > rule.condition.size()
            || (rule.requiredFlagIndex != kUnavailableFlagIndex
                && rule.requiredFlagIndex >= unlocks::kAccountFlagCapacity)
            || !std::all_of(rule.condition.begin(),
                            rule.condition.begin() + rule.conditionCount,
                            unlocks::valid)) {
            return false;
        }
    }
    return true;
}

/** Replaces the complete rule list in one step. */
bool replace(std::span<const Rule> rules) noexcept {
    if (!valid(rules)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    if (!g_rules.replace(rules)) {
        return false;
    }
    g_unsupported = false;
    return true;
}

/** Finds one plug's row by binary search over the ascending plug order. */
bool find(std::uint16_t plugDefinitionIndex, Rule& rule) noexcept {
    rule = {};
    const std::shared_lock guard(g_lock);
    const auto rows = g_rules.rows();
    const auto at = std::lower_bound(
        rows.begin(), rows.end(), plugDefinitionIndex, [](const Rule& row, std::uint16_t key) {
            return row.plugDefinitionIndex < key;
        });
    if (at == rows.end() || at->plugDefinitionIndex != plugDefinitionIndex) {
        return false;
    }
    rule = *at;
    return true;
}

/** Copies every rule row in plug order. */
bool snapshot(std::span<Rule> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_rules.snapshot(output, count);
}

/** @return The rule row count, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_rules.count();
}

} // namespace sunrise::state::build_data::items::insertion_rules
