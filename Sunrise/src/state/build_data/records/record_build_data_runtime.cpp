#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "record_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the whole native record table is published. */
bool record_definitions_ready() noexcept {
    return records::count() != 0;
}

/** Publishes one complete dense record-to-completion-flag table. */
bool publish_record_definitions(std::span<const records::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active() && records::valid(definitions)
           && transaction.finish(records::replace(definitions), records::clear);
}

/** Resolves the native record row an opcode-1801 claim names. */
bool find_record_definition(std::uint16_t definitionIndex,
                            records::Definition& definition) noexcept {
    return records::find(definitionIndex, definition);
}

} // namespace sunrise::state::build_data
