#pragma once

#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../state/coop/reward_mail.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

/**
 * Advances the selected character's held bounties from one accepted kill report. A guest's kill is
 * queued for the guest's own install, which credits it there.
 * @param incident Outer-valid incident; any other target is left alone.
 */
void apply_kill_progress(
    const middleware::bap::activity_message::incident::Incident& incident) noexcept;

/** Credits a kill the host queued for this install. The caller binds the local account. */
void apply_mailed_kill(const state::coop::reward_mail::Event& event) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
