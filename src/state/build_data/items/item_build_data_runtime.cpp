#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"

namespace sunrise::state::build_data {

/** @return True when the whole item definition table is in State. */
bool item_definitions_ready() noexcept {
    return items::count() != 0;
}

/** Publishes one dense item table and saves every ready build-data domain. */
bool publish_item_definitions(std::span<const items::Definition> definitions) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active() && transaction.finish(items::replace(definitions), items::clear);
}

/** Finds one authored item hash, only after a complete item table is published. */
bool find_item_definition(std::uint32_t definitionHash,
                          std::uint8_t bucketId,
                          items::Definition& definition) noexcept {
    definition = {};
    return item_definitions_ready() && items::find(definitionHash, bucketId, definition);
}

/** Finds one authored hash without needing a native bucket in settings. */
bool find_item_definition_hash(std::uint32_t definitionHash,
                               items::Definition& definition) noexcept {
    definition = {};
    return item_definitions_ready() && items::find_hash(definitionHash, definition);
}

} // namespace sunrise::state::build_data
