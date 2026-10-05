#include "../../transactions/internal.h"
#include "../member_directory_builder.h"
#include "internal.h"

namespace sunrise::state::activity::membership::transactions {

/** Captures one joined session and its shared transaction guards. */
const SessionRecord* prepare_base(const ActivityState& state,
                                  std::uint64_t primarySoid,
                                  std::uint64_t sessionId,
                                  PendingMutation& mutation) noexcept {
    const std::size_t target = activity::transactions::find_session(state, sessionId);
    if (target == kInvalidSessionSlot) {
        return nullptr;
    }
    const SessionRecord& record = state.sessions[target];
    if (!record.joined || record.joinedRevision == kInvalidRevision
        || record.recordRevision == kInvalidRevision) {
        return nullptr;
    }
    const auto context = member_context();
    mutation.memberRow = record.sharedMembers
                             ? member_row(record,
                                          context.sessionId == sessionId ? context.memberKey : 0,
                                          primarySoid)
                             : 0;
    if (!member_state(record, mutation.memberRow)) {
        return nullptr;
    }
    // The joined-row check above also makes the identity lookup succeed for a shared row.
    const auto* identity =
        record.sharedMembers ? member_identity(record, mutation.memberRow) : nullptr;
    if (record.sharedMembers && identity == nullptr) {
        return nullptr;
    }
    mutation.expectedMemberKey = identity != nullptr ? identity->memberKey : record.memberKey;
    mutation.sessionId = sessionId;
    mutation.peerSnapshot = record.peerReservations;
    if (identity != nullptr) {
        mutation.memberDirectory = member_directory(record, *identity);
    }
    mutation.expectedStateRevision = state.stateRevision;
    mutation.expectedRecordRevision = record.recordRevision;
    mutation.expectedPrimarySoid = primarySoid;
    mutation.targetSlot = target;
    mutation.prepared = true;
    return &record;
}

/** Advances root and record revisions after one stored membership change. */
void publish_change(ActivityState& state, SessionRecord& record) noexcept {
    ++state.stateRevision;
    record.recordRevision = state.stateRevision;
}

} // namespace sunrise::state::activity::membership::transactions
