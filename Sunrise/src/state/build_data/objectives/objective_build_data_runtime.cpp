#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "objective_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the objective table is published. */
bool objective_definitions_ready() noexcept {
    return objectives::ready();
}

/** Checks an objective table against the catalog's publication rules. */
bool valid_objective_definitions(objectives::View definitions) noexcept {
    return objectives::valid(definitions);
}

/** Publishes the objective table and its instruction bank in one step. */
bool publish_objective_definitions(objectives::View definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(objectives::replace(definitions), objectives::clear);
}

/** Copies one objective row with its programs. */
bool find_objective_definition(std::uint16_t index, objectives::Definition& output) noexcept {
    return objectives::find(index, output);
}

} // namespace sunrise::state::build_data
