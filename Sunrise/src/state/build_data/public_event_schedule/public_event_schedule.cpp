#include <mutex>

#include "definition.h"

namespace sunrise::state::build_data::public_event_schedule {
namespace {
std::mutex g_mutex;
Table g_table{};

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
    const std::lock_guard lock(g_mutex);
    if (g_table.rowCount != 0) {
        return false;
    }
    g_table = value;
    return true;
}

bool ready() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_table.rowCount != 0;
}

const Table* snapshot() noexcept {
    static constexpr Table empty{};
    const std::lock_guard lock(g_mutex);
    return g_table.rowCount != 0 ? &g_table : &empty;
}

} // namespace sunrise::state::build_data::public_event_schedule
