#include "activity_destination_snapshot.h"

#include <Windows.h>

#include "../../runtime/storage/internal.h"
#include "../definition.h"
#include "activity_destination_validation.h"

namespace sunrise::state::activity::destination {

/** Copies the destination committed with one activity session. */
bool snapshot(std::uint64_t sessionId, DestinationSelection& output) noexcept {
    output = {};
    if (sessionId == kAbsentSessionId) {
        return false;
    }

    DestinationSelection selected{};
    bool found = false;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    for (const SessionRecord& record : runtime::storage::g_state.activity.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            selected = record.destination;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    if (!found || !valid(selected)) {
        return false;
    }
    output = selected;
    return true;
}

} // namespace sunrise::state::activity::destination
