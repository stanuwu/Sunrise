#pragma once

#include <cstdint>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../state/activity/destination/definition.h"
#include "../../../../../state/coop/reward_mail.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

namespace reward_mail = state::coop::reward_mail;

/** The instantiated region's activity session and the destination State holds for it. */
struct LiveDestination {
    std::uint64_t sessionId{};
    state::activity::destination::DestinationSelection selection{};

    /** @return The destination's package name, empty when the session has no destination. */
    [[nodiscard]] std::string_view package_name() const noexcept;
};

/**
 * Reads the destination of the instantiated region. A report can arrive on the private activity
 * while its object belongs to the public region, so this is not the delivering session's.
 */
[[nodiscard]] LiveDestination live_destination() noexcept;

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