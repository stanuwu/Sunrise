#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "starter_set_catalog.h"

namespace sunrise::state::build_data {

/** @return True once the new-character table, empty or not, is in State. */
bool starter_sets_ready() noexcept {
    return starter_sets::ready();
}

/** Publishes the new-character table, and writes the cache once every domain is ready. */
bool publish_starter_sets(std::span<const starter_sets::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(starter_sets::replace(definitions), starter_sets::clear);
}

/** Finds the items the packages give a new character of one class. */
bool find_starter_set(std::uint8_t characterClass, starter_sets::Definition& definition) noexcept {
    return starter_sets::find(characterClass, definition);
}

} // namespace sunrise::state::build_data
