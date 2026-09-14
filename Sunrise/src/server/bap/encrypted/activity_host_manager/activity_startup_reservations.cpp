#include "activity_startup_reservations.h"

#include "../../../../state/activity/reservations/runtime.h"
#include "../activity_message/native_member_identity.h"

namespace sunrise::server::bap::encrypted::activity_host_manager {
PendingStartupReservations prepare_startup_reservations(
    const middleware::bap::activity_host_manager::request::selection::StartupReservations& parsed,
    std::uint64_t publisherAccount) noexcept {
    PendingStartupReservations batch{};
    if (!parsed.valid || parsed.count > batch.identities.size() || !publisherAccount) {
        return batch;
    }
    batch.publisherAccount = publisherAccount;
    batch.count = parsed.count;
    unsigned publisherCount{};
    for (std::size_t i = 0; i < batch.count; ++i) {
        auto& identity = batch.identities[i];
        identity = activity_message::native_member_identity(parsed.identities[i]);
        if (identity.accountSoid == publisherAccount && identity.memberKey) {
            ++publisherCount;
            batch.publisherMemberKey = identity.memberKey;
            batch.publisherCharacter = identity.opaqueSoid;
        }
    }
    if (batch.count && publisherCount != 1) {
        return {};
    }
    batch.valid = true;
    return batch;
}

bool admit_startup_reservations(PendingStartupReservations& pending,
                                std::uint64_t sessionId,
                                std::uint64_t createdRevision,
                                std::uint64_t publisherAccount,
                                std::uint64_t memberKey,
                                std::uint64_t characterSoid) noexcept {
    const auto batch = pending;
    pending = {};
    if (!batch.valid || !batch.count || !sessionId || !createdRevision
        || batch.sessionId != sessionId || batch.createdRevision != createdRevision
        || batch.publisherAccount != publisherAccount || batch.publisherMemberKey != memberKey
        || batch.publisherCharacter != characterSoid) {
        return false;
    }
    state::activity::reservations::PendingMutation mutation{};
    return state::activity::reservations::prepare_admit(
               sessionId, std::span(batch.identities).first(batch.count), mutation)
           && state::activity::reservations::commit(mutation);
}
} // namespace sunrise::server::bap::encrypted::activity_host_manager
