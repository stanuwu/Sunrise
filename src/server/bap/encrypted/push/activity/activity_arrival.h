#pragma once

#include <cstdint>
#include <string_view>

#include "../../../../../state/activity/defaults/definition.h"
#include "../../../../../state/activity/destination/definition.h"
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

} // namespace sunrise::server::bap::encrypted::push::activity
