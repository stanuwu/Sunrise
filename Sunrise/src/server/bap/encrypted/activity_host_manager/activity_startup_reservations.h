#pragma once

#include "../../../../middleware/bap/activity_host_manager/request/selection/startup_reservations.h"
#include "../../../../state/activity/reservations/definition.h"

namespace sunrise::server::bap::encrypted::activity_host_manager {

/** Safe identities retained only from a successful allocation until its own native join. */
struct PendingStartupReservations final {
    std::array<state::activity::membership::Identity, 32> identities{};
    std::uint64_t sessionId{};
    std::uint64_t createdRevision{};
    std::uint64_t publisherAccount{};
    std::uint64_t publisherMemberKey{};
    std::uint64_t publisherCharacter{};
    std::uint8_t count{};
    bool valid{};
};

[[nodiscard]] PendingStartupReservations prepare_startup_reservations(
    const middleware::bap::activity_host_manager::request::selection::StartupReservations& parsed,
    std::uint64_t publisherAccount) noexcept;

/** Consumes the batch, including on refusal. Called only after the owner's native join commits. */
[[nodiscard]] bool admit_startup_reservations(PendingStartupReservations& pending,
                                              std::uint64_t sessionId,
                                              std::uint64_t createdRevision,
                                              std::uint64_t publisherAccount,
                                              std::uint64_t memberKey,
                                              std::uint64_t characterSoid) noexcept;
} // namespace sunrise::server::bap::encrypted::activity_host_manager
