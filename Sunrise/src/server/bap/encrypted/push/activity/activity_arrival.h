#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../../../state/activity/defaults/definition.h"
#include "../../../../../state/activity/definition.h"
#include "../../../../../state/activity/destination/definition.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/build_data/scenarios/definition.h"

namespace sunrise::server::bap::encrypted::push::activity {

/**
 * Finds the slice-set index a destination arrives in.
 * Order: authored override, the bubble the client named, the default destination's, then its first
 * live one. A fallback never crosses destinations; a foreign index loads wrong geometry silently.
 *
 * @param defaults Authored default destination and its numeric launch policy.
 * @param selection Destination the session committed, carrying any wire arrival hash or override.
 * @param name Destination package name.
 * @param layout Extracted layout for that name, or a zero-bubble layout when there is none.
 * @return The slice-set index to publish.
 */
[[nodiscard]] std::uint16_t
arrival_slice_set(const state::activity::defaults::DefaultDestination& defaults,
                  const state::activity::destination::DestinationSelection& selection,
                  std::string_view name,
                  const state::build_data::scenarios::Definition& layout) noexcept;

/** The region one session publishes, with the arrival slice set behind it. */
struct EffectiveRegion final {
    /** Region index. The roster and the citizen advertisement both publish this value. */
    std::int32_t index{};
    /** The destination's own arrival slice set. The spawn override always names it. */
    std::uint16_t arrival{};
    /** True when the client reported the region, false when the arrival stood in. */
    bool reported{};
};

/**
 * Resolves the one region a session publishes.
 * The client's report wins. Before the first report the destination's arrival slice set stands in.
 * @param binding Exact joined activity-session generation.
 * @return The published region index, its source, and the destination's arrival slice set.
 */
[[nodiscard]] EffectiveRegion
effective_region(const state::activity::SessionBinding& binding) noexcept;

/**
 * Resolves the region one prepared membership body publishes.
 * Staging runs before the commit, so committed State still names the region just left. The body
 * must name the delta's region or its advertisement fills the wrong record.
 * @param mutation Prepared membership operation, whose sparse input may carry a new region.
 * @param binding Exact joined activity-session generation used when the delta names no region.
 * @return The region this body publishes, its source, and the destination's arrival slice set.
 */
[[nodiscard]] EffectiveRegion
planned_region(const state::activity::membership::PendingMutation& mutation,
               const state::activity::SessionBinding& binding) noexcept;

/**
 * Resolves the region one private link's membership body publishes.
 * With no reported leg it is the private host row's, not the committed position of the activity
 * being left. The client clears its legs while waiting, so a cleared leg proves nothing.
 * @param mutation Prepared membership operation, whose sparse input may carry a new region.
 * @param binding Exact joined activity-session generation that owns the private host row.
 * @return The region this body publishes, its source, and the destination's arrival slice set.
 */
[[nodiscard]] EffectiveRegion
private_planned_region(const state::activity::membership::PendingMutation& mutation,
                       const state::activity::SessionBinding& binding) noexcept;

/**
 * Lists the regions one membership body advertises a host for, published region first.
 * A fast travel disconnects its current host before it looks for the target, so the target's row
 * has to already be in the table when it looks.
 *
 * @param binding Exact joined activity-session generation.
 * @param regionIndex Region this body publishes.
 * @param output Caller storage. Its size caps the list.
 * @param count Receives the filled entries, which is zero when the binding is stale.
 */
void directory_regions(const state::activity::SessionBinding& binding,
                       std::int32_t regionIndex,
                       std::span<std::int32_t> output,
                       std::size_t& count) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity
