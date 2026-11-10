#pragma once

#include <cstdint>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/incident.h"
#include "../../../../../state/activity/destination/definition.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

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
 * Applies the state change one accepted collectible report implies.
 * @param incident Outer-valid incident whose targets name the reported object.
 */
void apply_incident_grants(
    const middleware::bap::activity_message::incident::Incident& incident) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
