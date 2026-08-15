#include "entitlement_runtime.h"

#include "validation.h"

namespace sunrise::state::entitlements {
namespace {

/** @return Process-wide policy storage, seeded with the bundled policy on first use. */
[[nodiscard]] Table& storage() noexcept {
    static Table table = authored();
    return table;
}

} // namespace

/** Publishes the immutable ownership policy for this process. */
bool publish(const Table& table) noexcept {
    if (!valid(table)) {
        return false;
    }
    storage() = table;
    return true;
}

/** @return The active ownership policy, or the bundled policy when none was published. */
const Table& get() noexcept {
    return storage();
}

/** Restores the bundled ownership policy. */
void clear() noexcept {
    storage() = authored();
}

} // namespace sunrise::state::entitlements
