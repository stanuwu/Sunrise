#include "vendor_reward_pool.h"

#include <algorithm>
#include <mutex>

namespace sunrise::state::vendor_rewards {
namespace {

std::mutex g_mutex;
std::array<Pool, kPackageHashes.size()> g_pools{};
bool g_ready = false;
bool g_settled = false;

/**
 * Refuses an incomplete or duplicate candidate set before publication.
 * @param pool One package and its class-specific gear candidates.
 * @return True when all three class lists hold supported unique hashes.
 */
bool valid(const Pool& pool) noexcept {
    if (std::find(kPackageHashes.begin(), kPackageHashes.end(), pool.packageHash)
        == kPackageHashes.end()) {
        return false;
    }
    for (std::size_t classIndex = 0; classIndex < pool.items.size(); ++classIndex) {
        const auto count = pool.counts[classIndex];
        if (count == 0 || count > kCandidateCapacity) {
            return false;
        }
        const auto begin = pool.items[classIndex].begin();
        for (std::size_t index = 0; index < count; ++index) {
            const auto hash = pool.items[classIndex][index];
            if (hash == 0
                || std::find(begin, begin + static_cast<std::ptrdiff_t>(index), hash)
                       != begin + static_cast<std::ptrdiff_t>(index)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/** @return True when supported package pools are available for claims. */
bool ready() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_ready;
}

/** @return True when extraction has finished, with or without supported pools. */
bool settled() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_settled;
}

/** An unsupported installed build must not block unrelated content loading. */
void settle_unavailable() noexcept {
    const std::lock_guard lock(g_mutex);
    g_settled = true;
}

/**
 * Publishes complete class-specific package membership together.
 * @param pools Exactly one pool per supported package.
 * @return False without changing the published set when validation fails.
 */
bool replace(std::span<const Pool> pools) noexcept {
    if (pools.size() != g_pools.size()) {
        return false;
    }
    for (std::size_t index = 0; index < pools.size(); ++index) {
        if (!valid(pools[index])
            || std::find_if(
                   pools.begin(),
                   pools.begin() + static_cast<std::ptrdiff_t>(index),
                   [&](const Pool& other) { return other.packageHash == pools[index].packageHash; })
                   != pools.begin() + static_cast<std::ptrdiff_t>(index)) {
            return false;
        }
    }
    const std::lock_guard lock(g_mutex);
    std::copy(pools.begin(), pools.end(), g_pools.begin());
    g_ready = true;
    g_settled = true;
    return true;
}

/**
 * Copies one published pool under the catalog lock.
 * @param packageHash Installed package item hash.
 * @param output Receives the pool on success; unchanged on failure.
 * @return False when no supported pool holds the package.
 */
bool find(std::uint32_t packageHash, Pool& output) noexcept {
    const std::lock_guard lock(g_mutex);
    if (!g_ready) {
        return false;
    }
    const auto found = std::find_if(g_pools.begin(), g_pools.end(), [&](const Pool& pool) {
        return pool.packageHash == packageHash;
    });
    if (found == g_pools.end()) {
        return false;
    }
    output = *found;
    return true;
}

/**
 * Invalid class selectors cannot inherit another class's armour candidates.
 * @param pool Installed package candidates.
 * @param characterClass Selected character class.
 * @return This class's candidates, or empty for an invalid class.
 */
std::span<const std::uint32_t> candidates(const Pool& pool,
                                          CharacterClass characterClass) noexcept {
    const auto index = static_cast<std::size_t>(characterClass);
    return index < pool.items.size() && pool.counts[index] <= kCandidateCapacity
               ? std::span<const std::uint32_t>{pool.items[index].data(), pool.counts[index]}
               : std::span<const std::uint32_t>{};
}

} // namespace sunrise::state::vendor_rewards
