#include "family0_source_seed.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../../../core/logging/log.h"

namespace sunrise::client::hooks::queuez::family0 {
namespace {

/** Fields of the queuez manager this hook reads. */
struct ManagerLayout {
    /** The account key the sweep itself passes to every family subscribe. */
    static constexpr std::size_t accountKey = 16;
};

/** Fields of the family-zero source list. The sweep diffs the whole fixed-size record. */
struct SourceListLayout {
    /** Entry count, held twice. The sweep treats both as the list length. */
    static constexpr std::size_t countA = 0;
    static constexpr std::size_t countB = 8;
    /** First entry, {u64 key, u16 mask}. */
    static constexpr std::size_t firstKey = 16;
    static constexpr std::size_t firstMask = 24;
};

/**
 * The smallest entry mask that passes the sweep's own filter without claiming the family-zero
 * subscription is already synced. That synced state is what the subscribe must reach.
 */
constexpr std::uint16_t kEntryMask = 1;
/** One seeded entry: one record per source key, and one is all the account needs. */
constexpr std::uint64_t kSeededCount = 1;
/** Bytes of the source list the producer owns, rebuilt whole so no stale entry survives. */
constexpr std::size_t kSourceListBytes = 0x210;

std::atomic<SourceList> g_sourceList{nullptr};
/** The account key, read from the manager rather than written by us, so it cannot disagree. */
std::atomic<std::uint64_t> g_accountKey{0};
std::atomic_bool g_reported{false};

} // namespace

/** @param getter The source-list getter we found, or null to withdraw it. */
void publish_source_list(SourceList getter) noexcept {
    g_sourceList.store(getter, std::memory_order_release);
}

/** Captures the account key the source-list seed needs. */
void learn_account_key(std::byte* manager) noexcept {
    if (manager == nullptr || g_accountKey.load(std::memory_order_acquire) != 0) {
        return;
    }
    std::uint64_t key = 0;
    std::memcpy(&key, manager + ManagerLayout::accountKey, sizeof key);
    if (key != 0) {
        g_accountKey.store(key, std::memory_order_release);
    }
}

/** Seeds the source list with the account key. Runs every tick, writes only when it differs. */
void seed_source_list() noexcept {
    const SourceList source = g_sourceList.load(std::memory_order_acquire);
    const std::uint64_t key = g_accountKey.load(std::memory_order_acquire);
    if (source == nullptr || key == 0) {
        return;
    }
    std::byte* const list = source();
    if (list == nullptr) {
        return;
    }
    std::uint64_t existing = 0;
    std::memcpy(&existing, list + SourceListLayout::firstKey, sizeof existing);
    if (existing == key) {
        return; // Already ours. Rewriting the same bytes would only churn the subscribe.
    }
    // The whole buffer is rebuilt, so an entry the producer left past the first cannot survive
    // as a key the sweep would go on to subscribe.
    std::array<std::byte, kSourceListBytes> seeded{};
    std::memcpy(seeded.data() + SourceListLayout::countA, &kSeededCount, sizeof kSeededCount);
    std::memcpy(seeded.data() + SourceListLayout::countB, &kSeededCount, sizeof kSeededCount);
    std::memcpy(seeded.data() + SourceListLayout::firstKey, &key, sizeof key);
    std::memcpy(seeded.data() + SourceListLayout::firstMask, &kEntryMask, sizeof kEntryMask);
    std::memcpy(list, seeded.data(), seeded.size());
    if (!g_reported.exchange(true, std::memory_order_relaxed)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=queuez stage=family0 result=seeded");
    }
}

/** Clears the captured key, the published getter, and the one-shot report. */
void reset() noexcept {
    g_sourceList.store(nullptr, std::memory_order_release);
    g_accountKey.store(0, std::memory_order_release);
    g_reported.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::queuez::family0
