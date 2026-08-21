#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "node_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the whole native node table is published. */
bool node_definitions_ready() noexcept {
    return nodes::count() != 0;
}

/** Publishes one complete dense node-to-value-slot table. */
bool publish_node_definitions(std::span<const nodes::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active() && nodes::valid(definitions)
           && transaction.finish(nodes::replace(definitions), nodes::clear);
}

} // namespace sunrise::state::build_data
