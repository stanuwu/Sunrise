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
} // namespace

/** Clears the reward rows under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_rewards.clear();
}

/** Checks one complete pass catalog; unavailable rows keep their native positions. */
bool valid(std::span<const Reward> rewards) noexcept {
    if (rewards.empty() || rewards.size() > kRewardCapacity) {
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
    return true;
}

/** Replaces the complete pass catalog in one step. */
bool replace(std::span<const Reward> rewards) noexcept {
    if (!valid(rewards)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_rewards.replace(rewards);
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

/** Copies every reward row in native reward order. */
bool snapshot(std::span<Reward> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_rewards.snapshot(output, count);
}

/** @return The reward row count, including unavailable rows, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_rewards.count();
}

} // namespace sunrise::state::build_data::season_pass
