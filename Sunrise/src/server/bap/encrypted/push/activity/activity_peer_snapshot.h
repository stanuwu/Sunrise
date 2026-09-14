#pragma once

#include "../../../../../middleware/bap/activity_message/replicate_membership.h"
#include "../../../../../state/activity/membership/member_directory.h"
#include "../../../../../state/activity/reservations/definition.h"

namespace sunrise::server::bap::encrypted::push {
/** Called under the BAP transaction lock with the committed native roster snapshot. */
void project_activity_peers(
    const state::activity::reservations::Roster& roster,
    middleware::bap::activity_message::replicate_membership::MembershipSnapshot& output) noexcept;
void project_activity_peers(
    const state::activity::membership::MemberDirectory& directory,
    middleware::bap::activity_message::replicate_membership::MembershipSnapshot& output) noexcept;
} // namespace sunrise::server::bap::encrypted::push
