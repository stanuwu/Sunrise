#include "activity_defaults_snapshot.h"

#include <Windows.h>

#include <algorithm>
#include <string_view>

#include "../../runtime/storage/internal.h"

namespace sunrise::state::activity::defaults {
namespace {

/** @return The authored row's package name as text. */
[[nodiscard]] std::string_view override_name(const ArrivalOverride& override) noexcept {
    return {override.name.data(), override.nameLength};
}

} // namespace

/** Copies the immutable activity defaults published with the root State. */
void snapshot(ActivityDefaults& output) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    output = runtime::storage::g_state.activity.defaults;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
}

/** Applies any authored arrival override for one destination onto its selection. */
void apply_arrival_override(const ActivityDefaults& defaults,
                            destination::DestinationSelection& selection) noexcept {
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    if (name.empty()) {
        return;
    }
    const std::size_t count = (std::min)(static_cast<std::size_t>(defaults.arrivalOverrideCount),
                                         defaults.arrivalOverrides.size());
    for (std::size_t index = 0; index < count; ++index) {
        const ArrivalOverride& row = defaults.arrivalOverrides[index];
        if (override_name(row) != name) {
            continue;
        }
        if (row.hasBubble) {
            selection.arrivalBubbleOverride = row.bubble;
            selection.hasArrivalBubbleOverride = true;
        }
        if (row.hasSpawnSetHash) {
            selection.spawnSetOverride = row.spawnSetHash;
            selection.hasSpawnSetOverride = true;
        }
        return;
    }
}

} // namespace sunrise::state::activity::defaults
