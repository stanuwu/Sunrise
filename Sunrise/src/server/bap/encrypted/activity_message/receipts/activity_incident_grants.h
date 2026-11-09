#pragma once

#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../state/coop/reward_mail.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

namespace reward_mail = state::coop::reward_mail;

/**
 * Applies the state change one accepted collectible report implies. A guest's report is queued
 * for the guest's own install, which grants it there.
 * @param incident Outer-valid incident whose targets name the reported object.
 */
void apply_incident_grants(
    const middleware::bap::activity_message::incident::Incident& incident) noexcept;

/** Grants one event the host queued for this install's own account. */
void apply_reward_event(const reward_mail::Event& event) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message::receipts