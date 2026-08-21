#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "../investment/investment.h"
#include "abilities/definition.h"
#include "collectibles/collectible_catalog.h"
#include "constants/definition.h"
#include "definition.h"
#include "hash_names/definition.h"
#include "inventory/buckets/definition.h"
#include "items/catalysts/definition.h"
#include "items/details/definition.h"
#include "items/item_catalog.h"
#include "items/socket_plugs/definition.h"
#include "material_requirements/material_requirement_catalog.h"
#include "progressions/definition.h"
#include "scenarios/definition.h"
#include "socket_entry_buckets/definition.h"
#include "socket_entry_lists/definition.h"
#include "spawn_sets/definition.h"
#include "vendors/definition.h"

namespace sunrise::state::build_data::items::catalysts {
struct Source;
}

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

/**
 * Finds one installed item by the native dense definition index used by Collections requests.
 * @param definitionIndex Native item-definition row index.
 * @param definition Receives the exact installed mapping.
 * @return True when the complete table is ready and contains the requested row.
 */
[[nodiscard]] bool find_item_definition_index(std::uint16_t definitionIndex,
                                              items::Definition& definition) noexcept;

/** @return True when the whole dense collectible definition table is in State. */
[[nodiscard]] bool collectible_definitions_ready() noexcept;

/**
 * Publishes the installed collectible ordinal-to-item table in one step.
 * @param definitions Complete dense rows extracted from the investment root's collectible table.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_collectible_definitions(std::span<const collectibles::Definition> definitions) noexcept;

/**
 * Resolves the native 15-bit collectible index carried by a Collections acquire request.
 * @param collectibleIndex Native collectible row ordinal.
 * @param itemDefinitionIndex Receives the installed item-definition row, or the unavailable
 * sentinel on failure.
 * @return True when both the complete table and an item link exist for this collectible.
 */
[[nodiscard]] bool
find_collectible_item_definition_index(std::uint16_t collectibleIndex,
                                       std::uint16_t& itemDefinitionIndex) noexcept;

/** Finds one complete installed collectible row, including its acquisition material set. */
[[nodiscard]] bool find_collectible_definition(std::uint16_t collectibleIndex,
                                               collectibles::Definition& definition) noexcept;

/** @return True when every installed material-requirement set is available by native ordinal. */
[[nodiscard]] bool material_requirement_sets_ready() noexcept;

/** Publishes the complete dense native material-requirement table. */
[[nodiscard]] bool publish_material_requirement_sets(
    std::span<const material_requirements::Definition> definitions) noexcept;

/** Resolves one authored material-requirement set without embedding any prices in code. */
[[nodiscard]] bool
find_material_requirement_set(std::uint16_t requirementSetIndex,
                              material_requirements::Definition& definition) noexcept;

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

/** @return True when the exact installed ordinary-socket plug relation is in State. */
[[nodiscard]] bool socket_plug_rules_ready() noexcept;

/**
 * Publishes exact per-item, per-lane plug pools extracted from the installed packages.
 * @param rules Strictly item/lane-ordered rules.
 * @param pools Deduplicated contiguous pool ranges, beginning with the empty pool.
 * @param members Flat sorted plug-definition indices.
 * @return True when the relation and its item/detail links validate and any cache write succeeds.
 */
[[nodiscard]] bool
publish_socket_plug_rules(std::span<const items::socket_plugs::Rule> rules,
                          std::span<const items::socket_plugs::Pool> pools,
                          std::span<const items::socket_plugs::Member> members) noexcept;

/**
 * Answers whether one installed plug definition is valid for one exact ordinary socket lane.
 * Missing or malformed relations fail closed.
 */
[[nodiscard]] bool is_socket_plug_allowed(std::uint16_t itemDefinitionIndex,
                                          std::uint8_t lane,
                                          std::uint16_t plugDefinitionIndex) noexcept;

/**
 * Walks every plug one exact ordinary socket lane accepts. Missing relations fail closed.
 * @return True when the lane has a pool and the visitor saw every member.
 */
[[nodiscard]] bool visit_socket_plug_pool(std::uint16_t itemDefinitionIndex,
                                          std::uint8_t lane,
                                          items::socket_plugs::MemberVisitor visitor,
                                          void* context) noexcept;

/** @return True when one plug definition occurs in any installed ordinary-socket plug pool. */
[[nodiscard]] bool is_socket_plug_pooled(std::uint16_t plugDefinitionIndex) noexcept;

/** @return True when the build-scoped exotic catalyst catalog is ready. */
[[nodiscard]] bool exotic_catalysts_ready() noexcept;

/**
 * Derives released and placeholder catalysts from one complete package pass.
 * The active PE identity supplies the build fingerprint.
 * @param source Complete installed item, detail, and socket-pool domains.
 * @param output Fixed storage that receives the catalyst catalog.
 * @param count Receives the used output row count.
 * @param report Receives catalog counts and the first unsafe released relation.
 * @return True when the complete catalog is safe for publication.
 */
[[nodiscard]] bool derive_exotic_catalysts(const items::catalysts::Source& source,
                                           std::span<items::catalysts::Definition> output,
                                           std::size_t& count,
                                           items::catalysts::Report& report) noexcept;

/**
 * @param source Complete installed item, detail, and socket-pool domains.
 * @param definitions Complete build-derived catalyst catalog.
 * @return True when validation, publication, and any due cache write succeed.
 */
[[nodiscard]] bool
publish_exotic_catalysts(const items::catalysts::Source& source,
                         std::span<const items::catalysts::Definition> definitions) noexcept;

/**
 * @param itemDefinitionIndex Native item definition index.
 * @param flags Candidate accumulated item-state bits.
 * @param plugs Candidate ordinary socket plugs.
 * @return Completed, unchanged, or failed without a partial change.
 */
[[nodiscard]] items::catalysts::ApplyResult
complete_exotic_catalyst(std::uint16_t itemDefinitionIndex,
                         std::uint32_t& flags,
                         std::span<std::optional<std::uint16_t>> plugs) noexcept;

/**
 * Adds all released catalyst acquisition and completion overrides to one Family-5 snapshot.
 * @param family Candidate Family-5 state.
 * @return True when all overrides fit and the complete state commits.
 */
[[nodiscard]] bool complete_exotic_catalyst_investment(Family5State& family) noexcept;

/**
 * Raises every account objective required by a released legacy catalyst.
 * @param values Candidate account objective bank.
 * @return True when all derived objectives fit and apply.
 */
[[nodiscard]] bool complete_exotic_catalyst_objectives(std::span<std::int32_t> values) noexcept;

/**
 * Resolves the item row that supplies one socketed catalyst's native perks and stat changes.
 * @param itemDefinitionIndex Native weapon definition index.
 * @param socketLane Ordinary socket lane.
 * @param plugDefinitionIndex Socketed plug definition index.
 * @return Effective definition index, or the input plug when no released completion matches.
 */
[[nodiscard]] std::uint16_t
resolve_exotic_catalyst_effect(std::uint16_t itemDefinitionIndex,
                               std::uint8_t socketLane,
                               std::uint16_t plugDefinitionIndex) noexcept;

/**
 * @param itemDefinitionIndex Native item definition index.
 * @param socketLane Ordinary socket lane.
 * @return True when the catalog owns this catalyst socket lane.
 */
[[nodiscard]] bool is_exotic_catalyst_lane(std::uint16_t itemDefinitionIndex,
                                           std::uint8_t socketLane) noexcept;

/** @param enabled True to complete released catalysts during item resolution. */
void set_exotic_catalyst_completion_enabled(bool enabled) noexcept;

/**
 * Answers whether one profile definition needs an item-instance resident so the native socket
 * action route can materialize it. Only stackable installed socket plugs in the supported mod and
 * shader profile buckets qualify; currency/material/intrinsic rows do not.
 */
[[nodiscard]] bool is_profile_action_source(std::uint16_t itemDefinitionIndex,
                                            std::uint8_t bucketId) noexcept;

/**
 * Answers whether applying one plug spends a stack the account has to hold.
 * @param itemDefinitionIndex Installed plug-definition row.
 * @param bucketId Installed profile bucket the plug belongs to.
 * @return True only for a shader Collections can grant. An ornament stays owned once applied,
 *         and a socket's default plug belongs to no stack at all.
 */
[[nodiscard]] bool is_consumed_on_apply(std::uint16_t itemDefinitionIndex,
                                        std::uint8_t bucketId) noexcept;

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

/**
 * Drops the published ability bucket domain so the next investment refresh slice rebuilds it.
 * A committed subclass ability-entry change makes the published rows stale for their character,
 * since they were keyed by the selection in place when the domain was first built.
 */
void invalidate_ability_buckets() noexcept;

/**
 * @return True when at least one socket-entry list's resolved bucket destinations are published.
 * Never part of the on-disk content cache: it is small and cheap to recompute, so a warm boot that
 * skips re-extraction must not leave it empty for the whole session.
 */
[[nodiscard]] bool socket_entry_buckets_ready() noexcept;

/**
 * Publishes every socket-entry list's resolved per-entry ability-bucket destinations.
 * A derived cache of static content, so unlike the ability buckets above it never needs
 * invalidating: a subclass's entry table does not change after content extraction.
 * @param definitions Complete rows, one per socket-entry list that carries a super lane.
 * @return True when the rows pass the checks.
 */
[[nodiscard]] bool publish_socket_entry_buckets(
    std::span<const socket_entry_buckets::Definition> definitions) noexcept;

/**
 * Finds which of the 12 semantic ability buckets one socket entry resolves to.
 * @param socketEntryListIndex Native socket-entry-list index of the subclass.
 * @param entryIndex The entry to look up.
 * @param bucket Receives the resolved destination, or the no-destination sentinel.
 * @return True when the list's row is published and the entry index is in range.
 */
[[nodiscard]] bool find_socket_entry_bucket(std::uint16_t socketEntryListIndex,
                                            std::uint8_t entryIndex,
                                            std::uint8_t& bucket) noexcept;

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

/** Number of subclass items every character class ships in this build. */
inline constexpr std::size_t kSubclassGroupSize = 3;

/**
 * Finds the 2 other subclasses that share one character class with a known member.
 * The installed manifest lists every subclass item as one dense run per class, in native
 * definition-index order, so the run holding a known member gives every other member.
 * @param memberDefinitionIndex Native item-definition index of one subclass in the class.
 * @param group Receives the 3 member indices, in native definition-index order.
 * @return True when every subclass item was found and `memberDefinitionIndex` is one of them.
 */
[[nodiscard]] bool
find_subclass_group(std::uint16_t memberDefinitionIndex,
                    std::array<std::uint16_t, kSubclassGroupSize>& group) noexcept;

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
 * @param points Complete point bank in extraction order, which may be empty.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool publish_spawn_sets(std::span<const spawn_sets::Stem> stems,
                                      std::span<const spawn_sets::NameHash> nameHashes,
                                      std::span<const spawn_sets::Point> points) noexcept;

/**
 * Finds the spawn point of one map-package stem nearest a world position.
 * @param stem Normalized stem a destination row carries.
 * @param position World position to measure from.
 * @param point Receives the nearest point and the set it belongs to.
 * @param distance Receives the distance to it, in world units.
 * @return True when the catalog is ready, holds the stem, and the stem owns a point.
 */
[[nodiscard]] bool
find_nearest_spawn_point(std::string_view stem,
                         const std::array<float, spawn_sets::kPositionComponents>& position,
                         spawn_sets::Point& point,
                         float& distance) noexcept;

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

/** @return True when the installed vendor index is in State. */
[[nodiscard]] bool vendor_catalog_ready() noexcept;

/**
 * Publishes the vendor index and every extracted vendor definition in one step.
 * @param index Complete index rows in ascending index order.
 * @param definitions Extracted definitions in ascending index order, which may be empty.
 * @param saleRows Complete flat sale bank in definition then row order.
 * @param installedRows Complete flat installed bank in definition then row order.
 * @return True when the rows pass the checks and any needed cache write succeeds.
 */
[[nodiscard]] bool
publish_vendor_catalog(std::span<const vendors::IndexEntry> index,
                       std::span<const vendors::Definition> definitions,
                       std::span<const vendors::SaleRow> saleRows,
                       std::span<const vendors::InstalledRow> installedRows) noexcept;

/**
 * Finds one vendor's index row, which carries the index the wire uses.
 * @param definitionHash Vendor definition hash.
 * @param entry Receives the matching row.
 * @return True when the catalog is ready and exactly one row carries the hash.
 */
[[nodiscard]] bool find_vendor_index(std::uint32_t definitionHash,
                                     vendors::IndexEntry& entry) noexcept;

/**
 * Finds one extracted vendor definition.
 * @param definitionHash Vendor definition hash.
 * @param definition Receives the matching definition.
 * @return True when the catalog is ready and holds that definition.
 */
[[nodiscard]] bool find_vendor_definition(std::uint32_t definitionHash,
                                          vendors::Definition& definition) noexcept;

/**
 * An unsupported catalyst build finishes without writing an incomplete cache.
 * @return True when required domains are ready and cache persistence safely finishes.
 */
[[nodiscard]] bool persist() noexcept;

} // namespace sunrise::state::build_data
