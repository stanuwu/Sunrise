#pragma once

#include "../../../../../middleware/bap/activity_message/incident.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

/**
 * Advances the selected character's held bounties from one accepted kill report.
 * @param incident Outer-valid incident; any other target is left alone.
 */
void apply_kill_progress(
    const middleware::bap::activity_message::incident::Incident& incident) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
