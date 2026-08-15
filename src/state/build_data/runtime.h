#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "abilities/definition.h"
#include "constants/definition.h"
#include "definition.h"
#include "hash_names/definition.h"
#include "inventory/buckets/definition.h"
#include "items/details/definition.h"
#include "items/item_catalog.h"
#include "progressions/definition.h"
#include "scenarios/definition.h"
#include "socket_entry_lists/definition.h"
#include "spawn_sets/definition.h"

namespace sunrise::state::build_data {

/**
 * Loads the one build-data cache next to the module, when there is one.
 * Once a snapshot is on disk, later replacements are refused until State restarts. A failed first
 * commit withdraws that publication and restores nothing. Keep readers out until this returns.
 * @param module Loaded Sunrise DLL module, or null to turn off disk saving.
 * @param configuredEquipmentHash Hash of the authored equipment. Not a secret.
 * @return True when the cache is missing, stale, or passes every check.
 */
[[nodiscard]] bool initialize(void* module, std::uint64_t configuredEquipmentHash) noexcept;

/** Clears all cached build mappings and persistence paths. */
void shutdown() noexcept;

/** @return True when named package mappings are complete in State. */
[[nodiscard]] bool named_catalog_ready() noexcept;

/**
 * Marks the already-filled named catalog complete, and saves once all data is ready.
 * Call only while named readiness is false. A failed first commit withdraws the target and does
 * not restore an earlier ready catalog.
 * @return True when the catalog is nonempty and any needed cache write succeeds.
 */
[[nodiscard]] bool publish_named_catalog() noexcept;

/** @return True when the whole item definition table is in State. */
[[nodiscard]] bool item_definitions_ready() noexcept;

/** @return Dense item-definition row count, read under the lock. */
[[nodiscard]] std::size_t item_definition_count() noexcept;

/**
 * Publishes the whole installed-build item definition table in one step.
 * Call only while item readiness is false. A failed first commit withdraws the target and does
 * not restore an earlier ready table.
 * @param definitions Dense native-index mappings extracted from the running client.
 * @return True when the mappings pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_item_definitions(std::span<const items::Definition> definitions) noexcept;

/**
 * Finds one authored item hash inside its expected inventory bucket.
 * @param definitionHash Authored item definition hash.
 * @param bucketId Expected inventory bucket id.
 * @param definition Receives the one matching native definition.
 * @return True when the item data is ready and exactly one mapping matches.
 */
[[nodiscard]] bool find_item_definition(std::uint32_t definitionHash,
                                        std::uint8_t bucketId,
                                        items::Definition& definition) noexcept;

/**
 * Finds an authored base-item or plug hash without storing a native index in settings.
 * @param definitionHash Authored item or plug definition hash.
 * @param definition Receives the one matching installed-build mapping.
 * @return True when the item data is ready and exactly one native row matches.
 */
[[nodiscard]] bool find_item_definition_hash(std::uint32_t definitionHash,
                                             items::Definition& definition) noexcept;

/** @return True when a complete configured-detail domain, empty or not, is published. */
[[nodiscard]] bool configured_item_details_ready() noexcept;

/**
 * Publishes configured item details. An empty domain is a complete one.
 * Call only while detail readiness is false. A failed first commit withdraws the target and does
 * not restore an earlier ready domain.
 * @param definitions Complete installed-build details for configured authored items.
 * @return True when the links pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_configured_item_details(std::span<const items::details::Definition> definitions) noexcept;

/**
 * Finds one configured item detail by native definition index.
 * @param definitionIndex Native installed-build item index.
 * @param definition Receives the matching configured detail.
 * @return True when the detail data is ready and holds that row.
 */
[[nodiscard]] bool find_configured_item_detail(std::uint16_t definitionIndex,
                                               items::details::Definition& definition) noexcept;

/** @return True when the whole progression definition table is in State. */
[[nodiscard]] bool progression_definitions_ready() noexcept;

/**
 * Publishes the whole progression definition table in one step.
 * @param definitions Dense rows in native definition order.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_progression_definitions(std::span<const progressions::Definition> definitions) noexcept;

/**
 * Lists the definition index each slot of one scope's progression array carries.
 * @param scope Replicated object owning the array.
 * @param output Slot storage, one entry per slot the array holds.
 * @param count Receives the number of keyed slots.
 * @return True when the table is ready and every keyed slot fits.
 */
[[nodiscard]] bool find_progression_slots(progressions::Scope scope,
                                          std::span<std::uint16_t> output,
                                          std::size_t& count) noexcept;

/** @return True when a complete ability bucket domain, empty or not, is published. */
[[nodiscard]] bool ability_buckets_ready() noexcept;

/**
 * Publishes the ability buckets every configured subclass and ability selection publishes.
 * @param definitions Complete rows, or an empty complete domain.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_ability_buckets(std::span<const abilities::Definition> definitions) noexcept;

/**
 * Finds the buckets one subclass publishes under one ability selection.
 * @param socketEntryListIndex Native socket-entry-list index of the subclass.
 * @param selection The character's 5 selected socket entries.
 * @param definition Receives the matching row.
 * @return True when the domain is ready and holds that exact key.
 */
[[nodiscard]] bool find_ability_buckets(std::uint16_t socketEntryListIndex,
                                        const abilities::Selection& selection,
                                        abilities::Definition& definition) noexcept;

/** @return True when the installed investment constants are in State. */
[[nodiscard]] bool investment_constants_ready() noexcept;

/**
 * Publishes the stat rows read from the installed investment constants blob.
 * @param value Constants extracted from the blob.
 * @return True when the row is extracted and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_investment_constants(const constants::InvestmentConstants& value) noexcept;

/**
 * Reads the published investment constants.
 * @param value Receives the published stat rows.
 * @return True when the constants are in State.
 */
[[nodiscard]] bool find_investment_constants(constants::InvestmentConstants& value) noexcept;

/** @return True when the whole inventory-bucket descriptor table is in State. */
[[nodiscard]] bool inventory_bucket_descriptors_ready() noexcept;

/**
 * Publishes the whole inventory-bucket routing table in one step.
 * Call only while bucket readiness is false. A failed first commit withdraws the target and does
 * not restore an earlier ready table.
 * @param descriptors Installed-build bucket descriptors.
 * @return True when the descriptors pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool publish_inventory_bucket_descriptors(
    std::span<const inventory::buckets::Descriptor> descriptors) noexcept;

/**
 * Finds one inventory-bucket descriptor by native bucket id.
 * @param bucketId Native inventory-bucket id.
 * @param descriptor Receives the matching routing descriptor.
 * @return True when the bucket data is ready and holds that row.
 */
[[nodiscard]] bool
find_inventory_bucket_descriptor(std::uint8_t bucketId,
                                 inventory::buckets::Descriptor& descriptor) noexcept;

/** @return True when the whole dense socket-entry-list table is in State. */
[[nodiscard]] bool socket_entry_lists_ready() noexcept;

/** @return Dense socket-entry-list row count, read under the lock. */
[[nodiscard]] std::size_t socket_entry_list_count() noexcept;

/**
 * Publishes the whole dense socket-entry-list table in one step.
 * Call only while socket-list readiness is false. A failed first commit withdraws the target and
 * does not restore an earlier ready table.
 * @param definitions Installed-build socket-entry-list definitions.
 * @param entryTables Per-entry inputs for the lists that carry a super lane.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_socket_entry_lists(std::span<const socket_entry_lists::Definition> definitions,
                           std::span<const socket_entry_lists::EntryTable> entryTables) noexcept;

/**
 * Finds one list's per-entry selection inputs.
 * @param definitionIndex Native socket-entry-list index.
 * @param table Receives the matching row.
 * @return True when a table is kept for that list.
 */
[[nodiscard]] bool find_socket_entry_table(std::uint16_t definitionIndex,
                                           socket_entry_lists::EntryTable& table) noexcept;

/**
 * Finds one socket-entry-list definition by native definition index.
 * @param definitionIndex Native socket-entry-list index.
 * @param definition Receives the matching installed-build row.
 * @return True when the socket-list data is ready and holds that row.
 */
[[nodiscard]] bool find_socket_entry_list(std::uint16_t definitionIndex,
                                          socket_entry_lists::Definition& definition) noexcept;

/** @return True when a complete destination-layout domain, empty or not, is published. */
[[nodiscard]] bool scenario_layouts_ready() noexcept;

/**
 * Publishes the extracted destination layouts and their roster groups in one step.
 * @param definitions Complete rows swept from the installed packages.
 * @param groups Complete roster groups the same sweep reached.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_scenario_layouts(std::span<const scenarios::Definition> definitions,
                         std::span<const scenarios::RosterGroup> groups) noexcept;

/**
 * Copies one roster group by the table index a destination row carries.
 * @param index Roster table index.
 * @param group Receives the group.
 * @return True when the domain is ready and the index is inside its table.
 */
[[nodiscard]] bool find_roster_group(std::size_t index, scenarios::RosterGroup& group) noexcept;

/** @return Number of published destination layouts, read under the lock. */
[[nodiscard]] std::size_t scenario_layout_count() noexcept;

/**
 * Copies every published destination layout.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count.
 * @return True when the domain is ready and output can hold every row.
 */
[[nodiscard]] bool snapshot_scenario_layouts(std::span<scenarios::Definition> output,
                                             std::size_t& count) noexcept;

/** @return True when a complete bubble-name table, empty or not, is published. */
[[nodiscard]] bool hash_names_ready() noexcept;

/**
 * Publishes the resolved bubble names in one step.
 * @param names Complete rows in ascending hash order, or an empty complete table.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool publish_hash_names(std::span<const hash_names::Name> names) noexcept;

/**
 * Finds one bubble's internal name by its hash.
 * @param hash Bubble name hash from a destination layout.
 * @param name Receives the matching row.
 * @return True when the table is ready and holds that exact hash.
 */
[[nodiscard]] bool find_hash_name(std::uint32_t hash, hash_names::Name& name) noexcept;

/** @return True when a complete spawn-set catalog, empty or not, is published. */
[[nodiscard]] bool spawn_sets_ready() noexcept;

/**
 * Publishes the spawn-set catalog extracted from the installed packages, in one step.
 * @param stems Complete stem rows in ascending name order, or an empty complete catalog.
 * @param nameHashes Complete flat bank in stem then hash order.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool publish_spawn_sets(std::span<const spawn_sets::Stem> stems,
                                      std::span<const spawn_sets::NameHash> nameHashes) noexcept;

/**
 * Copies the whole flat spawn-name hash bank.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count.
 * @return True when the catalog is ready and output can hold every row.
 */
[[nodiscard]] bool snapshot_spawn_name_hashes(std::span<spawn_sets::NameHash> output,
                                              std::size_t& count) noexcept;

/**
 * Finds the spawn sets one map-package stem declares.
 * @param stem Normalized stem a destination row carries.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count.
 * @return True when the catalog is ready, holds the stem, and the range fits.
 */
[[nodiscard]] bool find_spawn_sets(std::string_view stem,
                                   std::span<spawn_sets::NameHash> output,
                                   std::size_t& count) noexcept;

/**
 * Finds one destination's bubble layout by package name.
 * @param name Package name the client's selection carried.
 * @param definition Receives the matching row.
 * @return True when the domain is ready and holds that exact name.
 */
[[nodiscard]] bool find_scenario_layout(std::string_view name,
                                        scenarios::Definition& definition) noexcept;

/** @return True only when every domain is ready and any needed cache write succeeds. */
[[nodiscard]] bool persist() noexcept;

} // namespace sunrise::state::build_data
