#include "season_pass_catalog.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <shared_mutex>

#include "../../unlocks/definition.h"
#include "../../unlocks/unlocks_expression.h"
#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::season_pass {
namespace {
core::threading::SrwLock g_lock;
Table<Reward, kRewardCapacity> g_rewards;
Table<Package, kPackageCapacity> g_packages;
} // namespace

/** Clears the reward rows and wrapper packages under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_rewards.clear();
    g_packages.clear();
}

/** Checks one complete pass catalog; unavailable rows keep their native positions. */
bool valid(std::span<const Reward> rewards, std::span<const Package> packages) noexcept {
    if (rewards.empty() || rewards.size() > kRewardCapacity || packages.size() > kPackageCapacity) {
        return false;
    }
    constexpr auto kMaximumQuantity =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    for (const Reward& reward : rewards) {
        // An unavailable row keeps its native claim index and grants nothing.
        if (reward.itemHash == 0) {
            if (reward.quantity != 0 || reward.conditionCount != 0 || reward.socketCount != 0
                || reward.claimFlagIndex != kUnavailableFlagIndex) {
                return false;
            }
            continue;
        }
        if (reward.quantity == 0 || reward.quantity > kMaximumQuantity
            || (reward.claimFlagIndex != kUnavailableFlagIndex
                && reward.claimFlagIndex >= unlocks::kAccountFlagCapacity)
            || reward.socketCount > reward.sockets.size()
            || reward.conditionCount > reward.condition.size()
            || !std::all_of(reward.condition.begin(),
                            reward.condition.begin() + reward.conditionCount,
                            unlocks::valid)) {
            return false;
        }
    }
    return std::all_of(packages.begin(), packages.end(), [](const Package& package) {
        return package.definitionHash != 0 && package.itemCount != 0
               && package.itemCount <= kPackageItemCapacity;
    });
}

/** Replaces the complete pass catalog in one step. */
bool replace(std::span<const Reward> rewards, std::span<const Package> packages) noexcept {
    if (!valid(rewards, packages)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    const bool replaced = g_rewards.replace(rewards) && g_packages.replace(packages);
    if (!replaced) {
        g_rewards.clear();
        g_packages.clear();
    }
    return replaced;
}

/** Reads one available reward row by the index an opcode-2400 claim names. */
bool find(std::uint16_t rewardIndex, Reward& reward) noexcept {
    reward = {};
    const std::shared_lock guard(g_lock);
    if (rewardIndex >= g_rewards.count() || g_rewards.rows()[rewardIndex].itemHash == 0) {
        return false;
    }
    reward = g_rewards.rows()[rewardIndex];
    return true;
}

/** Finds the wrapper package one item hash opens into. */
bool find_package(std::uint32_t definitionHash, Package& package) noexcept {
    package = {};
    const std::shared_lock guard(g_lock);
    for (const Package& row : g_packages.rows()) {
        if (row.definitionHash == definitionHash) {
            package = row;
            return true;
        }
    }
    return false;
}

/** Copies every reward row in native reward order. */
bool snapshot(std::span<Reward> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_rewards.snapshot(output, count);
}

/** Copies every wrapper package in declared order. */
bool snapshot_packages(std::span<Package> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_packages.snapshot(output, count);
}

/** @return The reward row count, including unavailable rows, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_rewards.count();
}

} // namespace sunrise::state::build_data::season_pass
