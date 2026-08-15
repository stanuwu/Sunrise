#include "unlocks_runtime.h"

#include <Windows.h>

namespace sunrise::state::unlocks {
namespace {

Table g_table{};
SRWLOCK g_lock{SRWLOCK_INIT};

} // namespace

/** Publishes the immutable unlock policy for this process. */
void publish(const Table& table) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_table = table;
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return The active unlock policy, or an empty policy when none was published. */
const Table& get() noexcept {
    return g_table;
}

/** Restores the empty unlock policy. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_table = Table{};
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace sunrise::state::unlocks
