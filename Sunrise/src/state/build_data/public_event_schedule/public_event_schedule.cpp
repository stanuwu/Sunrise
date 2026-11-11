#include <atomic>
#include <memory>

#include "definition.h"

namespace sunrise::state::build_data::public_event_schedule {
namespace {
const SnapshotView g_empty = std::make_shared<const Table>();
std::atomic<SnapshotView> g_snapshot{g_empty};

/** Checks the external decoded table before publishing an immutable copy. */
[[nodiscard]] bool consistent(const Table& value) noexcept {
    if (value.rowCount == 0 || value.rowCount > kRowCapacity || value.entryCount > kEntryCapacity
        || value.groupCount > kGroupCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < value.rowCount; ++index) {
        const Row& row = value.rows[index];
        if (static_cast<std::size_t>(row.firstEntry) + row.entryCount > value.entryCount) {
            return false;
        }
    }
    return true;
}
} // namespace

bool publish(const Table& value) noexcept {
    if (!consistent(value)) {
        return false;
    }
    SnapshotView expected = g_empty;
    return g_snapshot.compare_exchange_strong(expected, std::make_shared<const Table>(value));
}

bool ready() noexcept {
    return g_snapshot.load(std::memory_order_acquire) != g_empty;
}

SnapshotView snapshot() noexcept {
    return g_snapshot.load(std::memory_order_acquire);
}

} // namespace sunrise::state::build_data::public_event_schedule
