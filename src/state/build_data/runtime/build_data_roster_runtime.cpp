#include "../runtime.h"
#include "../scenarios/scenario_catalog.h"

namespace sunrise::state::build_data {

/** Copies one roster group by the table index a destination row carries. */
bool find_roster_group(std::size_t index, scenarios::RosterGroup& group) noexcept {
    group = {};
    return scenario_layouts_ready() && scenarios::group(index, group);
}

} // namespace sunrise::state::build_data
