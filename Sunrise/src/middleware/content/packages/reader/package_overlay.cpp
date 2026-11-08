#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "overlay.h"

namespace sunrise::middleware::content::packages::reader::overlay {
namespace {

/** One published table, ordered by package id, then patch, so a lookup is a binary search. */
struct Table final {
    std::vector<File> files;
};

/** Readers load the current table without a lock; a publish swaps the pointer. */
std::atomic<const Table*> g_current{nullptr};
std::atomic<std::uint64_t> g_epoch{0};
/** Serializes publishers and owns every table published since the last clear. */
SRWLOCK g_writer{SRWLOCK_INIT};
std::vector<std::unique_ptr<const Table>> g_tables{};

/** @return True when the left row sorts before the right one. */
[[nodiscard]] bool before(const File& left, const File& right) noexcept {
    return left.packageId != right.packageId ? left.packageId < right.packageId
                                             : left.patchIndex < right.patchIndex;
}

/** @return True when both rows name the same generation. */
[[nodiscard]] bool same(const File& left, const File& right) noexcept {
    return left.packageId == right.packageId && left.patchIndex == right.patchIndex;
}

[[nodiscard]] std::span<const File> current() noexcept {
    const Table* table = g_current.load(std::memory_order_acquire);
    return table != nullptr ? std::span<const File>(table->files) : std::span<const File>{};
}

} // namespace

/** Two rows for one generation would make a read depend on row order, so they are refused. */
bool publish(std::span<const File> files) noexcept {
    std::unique_ptr<Table> table{};
    try {
        table = std::make_unique<Table>();
        table->files.assign(files.begin(), files.end());
    } catch (...) {
        return false;
    }
    std::sort(table->files.begin(), table->files.end(), &before);
    if (std::adjacent_find(table->files.begin(), table->files.end(), &same) != table->files.end()) {
        return false;
    }
    AcquireSRWLockExclusive(&g_writer);
    bool stored = true;
    try {
        g_tables.push_back(std::move(table));
    } catch (...) {
        stored = false;
    }
    if (stored) {
        g_current.store(g_tables.back().get(), std::memory_order_release);
        g_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    ReleaseSRWLockExclusive(&g_writer);
    return stored;
}

void clear() noexcept {
    AcquireSRWLockExclusive(&g_writer);
    g_current.store(nullptr, std::memory_order_release);
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    std::vector<std::unique_ptr<const Table>>{}.swap(g_tables);
    ReleaseSRWLockExclusive(&g_writer);
}

std::uint64_t epoch() noexcept {
    return g_epoch.load(std::memory_order_acquire);
}

/** Reads the current table; a publish during the call leaves this one alive. */
bool latest(std::uint16_t packageId, std::uint32_t& patchIndex) noexcept {
    const std::span<const File> files = current();
    const File last{.packageId = packageId,
                    .patchIndex = (std::numeric_limits<std::uint32_t>::max)()};
    const auto end = std::upper_bound(files.begin(), files.end(), last, &before);
    if (end == files.begin() || std::prev(end)->packageId != packageId) {
        return false;
    }
    patchIndex = std::prev(end)->patchIndex;
    return true;
}

bool find(std::uint16_t packageId, std::uint32_t patchIndex, Path& path) noexcept {
    const std::span<const File> files = current();
    const File probe{.packageId = packageId, .patchIndex = patchIndex};
    const auto found = std::lower_bound(files.begin(), files.end(), probe, &before);
    if (found == files.end() || !same(*found, probe)) {
        return false;
    }
    path = found->path;
    return true;
}

std::span<const File> files() noexcept {
    return current();
}

} // namespace sunrise::middleware::content::packages::reader::overlay
