#pragma once

#include "../../../../../middleware/bap/activity_message/replicate_membership.h"
#include "../../../../../state/activity/membership/member_directory.h"
#include "../../../../../state/activity/reservations/definition.h"

namespace sunrise::server::bap::encrypted::push {
/**
 * Projects committed identities regardless of optional profile metadata availability.
 * Called under the BAP transaction lock with the committed native roster snapshot.
 */
void project_activity_peers(
    const state::activity::reservations::Roster& roster,
    middleware::bap::activity_message::replicate_membership::MembershipSnapshot& output) noexcept;
/** As above, plus each peer's slot, current/pending region legs and transition token from the
    directory's own richer per-peer record. Empty output when the directory is not valid. */
void project_activity_peers(
    const state::activity::membership::MemberDirectory& directory,
    middleware::bap::activity_message::replicate_membership::MembershipSnapshot& output) noexcept;
} // namespace sunrise::server::bap::encrypted::push
