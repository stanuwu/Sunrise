#include "../inventory/buckets/inventory_bucket_catalog.h"
#include "../items/item_catalog.h"
#include "../runtime.h"
#include "../socket_entry_lists/socket_entry_list_catalog.h"
#include "persistence/publication_transaction.h"

namespace sunrise::state::build_data {

bool find_item_definition_index(std::uint16_t definitionIndex,
                                items::Definition& definition) noexcept {
    definition = {};
    return item_definitions_ready() && items::find_index(definitionIndex, definition);
}

/** @return True when the whole inventory-bucket descriptor table is in State. */
bool inventory_bucket_descriptors_ready() noexcept {
    return inventory::buckets::count() != 0;
}

/** Publishes one whole inventory-bucket descriptor table. */
bool publish_inventory_bucket_descriptors(
    std::span<const inventory::buckets::Descriptor> descriptors) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(inventory::buckets::replace(descriptors),
                                 inventory::buckets::clear);
}

/** Finds one inventory-bucket descriptor, once the whole table is in State. */
bool find_inventory_bucket_descriptor(std::uint8_t bucketId,
                                      inventory::buckets::Descriptor& descriptor) noexcept {
    descriptor = {};
    return inventory_bucket_descriptors_ready() && inventory::buckets::find(bucketId, descriptor);
}

/** @return True when the whole dense socket-entry-list table is in State. */
bool socket_entry_lists_ready() noexcept {
    return socket_entry_lists::count() != 0;
}

/** @return Dense item-definition row count, read under the lock. */
std::size_t item_definition_count() noexcept {
    return items::count();
}

/** @return Dense socket-entry-list row count, read under the lock. */
std::size_t socket_entry_list_count() noexcept {
    return socket_entry_lists::count();
}

/** Publishes one whole dense socket-entry-list table. */
bool publish_socket_entry_lists(
    std::span<const socket_entry_lists::Definition> definitions,
    std::span<const socket_entry_lists::EntryTable> entryTables) noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active() || !socket_entry_lists::valid(definitions)
        || !socket_entry_lists::valid_entry_tables(entryTables)) {
        return false;
    }
    return transaction.finish(socket_entry_lists::replace(definitions)
                                  && socket_entry_lists::replace_entry_tables(entryTables),
                              socket_entry_lists::clear);
}

/** Finds one list's per-entry selection inputs. */
bool find_socket_entry_table(std::uint16_t definitionIndex,
                             socket_entry_lists::EntryTable& table) noexcept {
    return socket_entry_lists::find_entry_table(definitionIndex, table);
}

/** Finds one socket-entry-list definition, once the whole table is in State. */
bool find_socket_entry_list(std::uint16_t definitionIndex,
                            socket_entry_lists::Definition& definition) noexcept {
    definition = {};
    return socket_entry_lists_ready() && socket_entry_lists::find(definitionIndex, definition);
}

} // namespace sunrise::state::build_data
