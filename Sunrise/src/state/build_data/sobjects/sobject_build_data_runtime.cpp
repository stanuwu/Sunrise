#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "sobject_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the whole incident-target table is published. */
bool sobject_definitions_ready() noexcept {
    return sobjects::count() != 0;
}

/** Publishes one complete incident-target table in one step. */
bool publish_sobject_definitions(std::span<const sobjects::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active() && sobjects::valid(definitions)
           && transaction.finish(sobjects::replace(definitions), sobjects::clear);
}

/** Finds one incident target by its wire index. */
bool find_sobject_definition(std::uint16_t targetIndex, sobjects::Definition& definition) noexcept {
    return sobjects::find(targetIndex, definition);
}

} // namespace sunrise::state::build_data
