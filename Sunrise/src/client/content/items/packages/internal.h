#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../../core/filesystem/path.h"
#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/abilities/definition.h"
#include "../../../../state/build_data/collectibles/collectible_catalog.h"
#include "../../../../state/build_data/constants/definition.h"
#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "../../../../state/build_data/items/details/definition.h"
#include "../../../../state/build_data/items/item_catalog.h"
#include "../../../../state/build_data/material_requirements/material_requirement_catalog.h"
#include "../../../../state/build_data/progressions/definition.h"
#include "../../../../state/build_data/runtime.h"

namespace sunrise::client::content::items::packages {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Equippable rows, their initial plugs, and authored override plugs. */
inline constexpr std::size_t kDetailCapacity =
    state::build_data::items::details::kDefinitionCapacity;

/** Native item indices selected for the deduplicated detail closure. */
using DetailRequests = std::bitset<state::build_data::items::kDefinitionCapacity>;

/** Authored definition hashes one pass looks for while walking the item index table. */
struct AuthoredHashes {
    std::array<std::uint32_t, kDetailCapacity> values{};
    std::size_t count{};
};

/** Everything the detail pass needs to read one requested definition from the packages. */
struct DetailSource {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    /** Item index table blob owning the located array. */
    std::span<const std::byte> table{};
    tables::Array array{};
    std::vector<std::byte>* definition{};
};

/** The container name is not always unique, so every match is a candidate. */
inline constexpr std::size_t kContainerCandidates = 16;

/** Lock-owned storage kept off the caller stack, shared by every stage of the pass. */
struct Storage {
    reader::Scratch scratch{};
    std::vector<std::byte> container{};
    std::vector<std::byte> child{};
    std::vector<std::byte> root{};
    std::vector<std::byte> definition{};
    /** Shared reusable/randomized plug-set table read from investment-root slot 51. */
    std::vector<std::byte> plugSetTable{};
    /** Compact 0..3 special plug-category code of every dense installed item row. */
    std::array<std::uint8_t, state::build_data::items::kDefinitionCapacity> specialPlugCategories{};
    /** Inventory routing rows held until the paired bucket-definition table is resolved. */
    std::array<state::build_data::inventory::buckets::Descriptor,
               state::build_data::inventory::buckets::kDescriptorCapacity>
        bucketRows{};
    std::size_t bucketCount{};
    std::array<std::int8_t, state::build_data::inventory::buckets::kDescriptorCapacity>
        equipmentSlotByBucket{};
    DetailRequests detailRequests{};
    std::array<std::uint16_t, kDetailCapacity> requestedDetailIndices{};
    std::vector<state::build_data::items::details::Definition> details{};
    AuthoredHashes authoredHashes{};
    std::vector<std::byte> abilityTable{};
    std::vector<std::byte> abilityPool{};
    std::array<state::build_data::abilities::Definition,
               state::build_data::abilities::kDefinitionCapacity>
        abilityRows{};
    std::array<state::build_data::socket_entry_buckets::Definition,
               state::build_data::socket_entry_buckets::kDefinitionCapacity>
        entryBucketRows{};
    std::array<state::build_data::progressions::Definition,
               state::build_data::progressions::kDefinitionCapacity>
        progressionRows{};
    /** Record rows held until the completion flag mapping is resolved. */
    std::array<state::build_data::records::Definition,
               state::build_data::records::kDefinitionCapacity>
        recordRows{};
    /** Node rows held until the value slot and owned records are resolved. */
    std::array<state::build_data::nodes::Definition,
               state::build_data::nodes::kDefinitionCapacity>
        nodeRows{};
    std::array<state::build_data::collectibles::Definition,
               state::build_data::collectibles::kDefinitionCapacity>
        collectibleRows{};
    std::array<state::build_data::material_requirements::Definition,
               state::build_data::material_requirements::kDefinitionCapacity>
        materialRequirementRows{};
    std::array<state::build_data::items::Definition, state::build_data::items::kDefinitionCapacity>
        rows{};
};

/**
 * Collects the authored equipment and plug hashes every configured character names.
 * Hashes are sorted so the index-table walk can test each row by binary search instead of
 * rescanning the whole authored set per row.
 * @param output Receives the sorted unique authored hashes.
 * @return True when the account is good and every hash fits.
 */
[[nodiscard]] bool collect_authored_hashes(AuthoredHashes& output) noexcept;

/** @param hashes Sorted authored hashes. @param hash Row hash. @return True when authored. */
[[nodiscard]] bool authored(const AuthoredHashes& hashes, std::uint32_t hash) noexcept;

/**
 * @param row Installed item row.
 * @return True when its bucket maps to an equipment slot
 * supported by the generated loadout.
 */
[[nodiscard]] bool equippable(const tables::items::Row& row) noexcept;

/** Publishes parsed inventory buckets after applying the extracted item-slot relation. */
[[nodiscard]] bool publish_buckets(Storage& storage) noexcept;

/**
 * Adds one definition index to the deduplicated requested set.
 * @param definitionIndex Native
 * item index.
 * @param requested Requested-set storage.
 */
void request(std::uint16_t definitionIndex, DetailRequests& requested) noexcept;

/**
 * Adds every socket lane's initial plug to the requested set.
 * A lane using native defaults falls back to this plug, so its detail must exist.
 * @param row Item row already read from its definition blob.
 * @param itemDefinitionCount Installed item-table bound.
 * @param requested Requested-set storage.
 */
void append_initial_plugs(const tables::items::Row& row,
                          std::uint64_t itemDefinitionCount,
                          DetailRequests& requested) noexcept;

/**
 * Materializes requested native indices in ascending order.
 * @param requested Deduplicated native-index set.
 * @param output Fixed detail-index storage.
 * @param count Receives the number of selected rows, or zero when output is too small.
 * @return True when every selected row fits.
 */
[[nodiscard]] bool materialize_requests(const DetailRequests& requested,
                                        std::span<std::uint16_t> output,
                                        std::size_t& count) noexcept;

/**
 * Reads one requested definition and turns it into its cached detail form.
 * @param source Located item index table and package reader state.
 * @param definitionIndex Native item index.
 * @param detail Receives the cached detail.
 * @param item Receives the raw definition row the detail was built from.
 * @return True when the row is found and its definition blob reads.
 */
[[nodiscard]] bool build_detail(const DetailSource& source,
                                std::uint16_t definitionIndex,
                                state::build_data::items::details::Definition& detail,
                                tables::items::Row& item) noexcept;

/**
 * Reads the stat rows the installed investment constants blob names.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the constants blob.
 * @param output Receives the extracted stat rows.
 * @return True when the blob reads and carries every named row.
 */
[[nodiscard]] bool
read_investment_constants(const reader::Source& source,
                          reader::Scratch& scratch,
                          std::span<const std::byte> root,
                          std::vector<std::byte>& blob,
                          state::build_data::constants::InvestmentConstants& output) noexcept;

/**
 * Builds the ability buckets one subclass publishes under one ability selection.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param listDefinition Socket-entry-list definition bytes of the subclass.
 * @param blob Scratch storage reused for every pool blob.
 * @param selection The character's 5 selected socket entries.
 * @param output Receives the 12 buckets and the overflow bank.
 * @return True when every selected entry reaches a bucket of its own.
 */
[[nodiscard]] bool build_ability_buckets(const reader::Source& source,
                                         reader::Scratch& scratch,
                                         std::span<const std::byte> listDefinition,
                                         std::vector<std::byte>& blob,
                                         const state::build_data::abilities::Selection& selection,
                                         state::build_data::abilities::Definition& output) noexcept;

/**
 * Builds one ability bucket row per distinct subclass and ability selection in use.
 * Two characters on the same subclass with the same ability selection publish identical
 * buckets, so the row is keyed by both and built once.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param root Investment root bytes.
 * @param table Scratch storage for the socket entry list table.
 * @param definition Scratch storage for one socket entry list definition.
 * @param blob Scratch storage reused for every pool blob.
 * @param output Row storage.
 * @param count Receives the number of rows built.
 * @return True when the table reads; a subclass that fails is skipped, not fatal.
 */
[[nodiscard]] bool build_character_abilities(
    const reader::Source& source,
    reader::Scratch& scratch,
    std::span<const std::byte> root,
    std::vector<std::byte>& table,
    std::vector<std::byte>& definition,
    std::vector<std::byte>& blob,
    std::span<state::build_data::abilities::Definition> output,
    std::size_t& count,
    std::span<state::build_data::socket_entry_buckets::Definition> entryBucketOutput,
    std::size_t& entryBucketCount) noexcept;

/**
 * Resolves which of the 12 semantic ability buckets every entry in one socket-entry list reaches.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param listDefinition One socket-entry list's definition bytes.
 * @param blob Scratch storage reused for every pool blob.
 * @param output Receives one resolved bucket per entry, or the no-destination sentinel.
 * @return True when the list's entries read.
 */
[[nodiscard]] bool resolve_entry_buckets(
    const reader::Source& source,
    reader::Scratch& scratch,
    std::span<const std::byte> listDefinition,
    std::vector<std::byte>& blob,
    std::array<std::uint8_t, state::build_data::socket_entry_lists::kEntryCapacity>&
        output) noexcept;

/**
 * Reads the progression definition table and the object array each definition routes to.
 * The table is inline rows, not index rows. The scope byte in a row picks the replicated object
 * holding that progression, and the row's place among rows of that scope is its slot there.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the table.
 * @param output Row storage in native definition order.
 * @param count Receives the number of rows read.
 * @return True when the table reads and every row fits.
 */
/**
 * Reads the records table and resolves each record's completion flag to a bank index.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the tables.
 * @param output Row storage in native record order.
 * @param count Receives the number of rows read.
 * @return True when both tables read and every row fits.
 */
/**
 * Reads the presentation node table and resolves each node's value slot and owned records.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the tables.
 * @param output Row storage in native node order.
 * @param count Receives the number of rows read.
 * @return True when both tables read and every row fits.
 */
[[nodiscard]] bool build_nodes(const reader::Source& source,
                               reader::Scratch& scratch,
                               std::span<const std::byte> root,
                               std::vector<std::byte>& blob,
                               std::span<state::build_data::nodes::Definition> output,
                               std::size_t& count) noexcept;

[[nodiscard]] bool build_records(const reader::Source& source,
                                 reader::Scratch& scratch,
                                 std::span<const std::byte> root,
                                 std::vector<std::byte>& blob,
                                 std::span<state::build_data::records::Definition> output,
                                 std::size_t& count) noexcept;

[[nodiscard]] bool build_progressions(const reader::Source& source,
                                      reader::Scratch& scratch,
                                      std::span<const std::byte> root,
                                      std::vector<std::byte>& blob,
                                      std::span<state::build_data::progressions::Definition> output,
                                      std::size_t& count) noexcept;

/**
 * Copies the block key material this pass borrows.
 * @param keys Receives the primary, alternate and nonce material.
 * @return True when the installed key table and the bootstrap token are both there.
 */
[[nodiscard]] bool collect_keys(reader::BlockKeys& keys) noexcept;

/**
 * Collects every catalogue tag carrying the container name.
 * @param candidates Receives the candidate tags.
 * @param count Receives the number of candidates.
 * @return True when the catalogue names at least one.
 */
[[nodiscard]] bool
investment_globals_tags(std::array<std::uint32_t, kContainerCandidates>& candidates,
                        std::size_t& count) noexcept;

/** @param directory Receives the installed packages directory. @return True when it exists. */
[[nodiscard]] bool package_directory(core::path::Buffer& directory) noexcept;

/** @param slot Requested-set position. @param definitionIndex Native item index that failed. */
void report_detail_failure(std::size_t slot, std::uint16_t definitionIndex) noexcept;

/** @param count Ability bucket rows the pass built, one per subclass and ability selection. */
void report_ability_count(std::size_t count) noexcept;

/** Reports a bounded number of exact subclass/ability extraction failures per process. */
void report_ability_failure(const char* stage,
                            std::size_t character,
                            std::size_t first,
                            std::size_t second) noexcept;

/** Reports requested, retained, and skipped detail closure rows. */
void report_detail_count(std::size_t requested, std::size_t built) noexcept;

/**
 * Reports the item index-table walk, including rows the pass could not read.
 * @param walked Table entries the pass visited.
 * @param rows Readable rows it retained.
 * @param skipped Entries omitted because their index row or definition was malformed.
 * @param truncated True when the walk stopped early because the row storage filled.
 */
void report_row_count(std::size_t walked,
                      std::size_t rows,
                      std::size_t skipped,
                      bool truncated) noexcept;

/** Reports the exact socket-rule, deduplicated-pool, member, and skipped-lane counts. */
void report_socket_plug_count(std::size_t rules,
                              std::size_t pools,
                              std::size_t members,
                              std::size_t skipped) noexcept;

/** Reports the validated installed bucket/equipment-slot coverage. */
void report_bucket_equipment_mapping(std::size_t mappedSlots) noexcept;

/** Reports the first rejected bucket-definition invariant in one process. */
void report_bucket_equipment_failure(const char* stage,
                                     std::size_t first,
                                     std::size_t second) noexcept;

/**
 * Reports the pass outcome once.
 * @param published Rows published, or zero on failure.
 * @param reason Stage the pass reached, named in the line.
 */
void report(std::size_t published, const char* reason) noexcept;

/**
 * Publishes the inventory bucket descriptors from the root's bucket table.
 * @param source Package source.
 * @param storage Pass storage.
 * @param root Investment root bytes.
 * @return True when the table reads and publishes.
 */
[[nodiscard]] bool build_buckets(const reader::Source& source,
                                 Storage& storage,
                                 std::span<const std::byte> root) noexcept;

/**
 * Publishes the socket entry list table from the root.
 * @param source Package source.
 * @param storage Pass storage.
 * @param root Investment root bytes.
 * @return True when the table reads and publishes.
 */
[[nodiscard]] bool build_socket_entry_lists(const reader::Source& source,
                                            Storage& storage,
                                            std::span<const std::byte> root) noexcept;

/**
 * Reads and publishes the root's dense collectible-to-item mapping table.
 * @param source Package source.
 * @param storage Pass storage, including scratch bytes and bounded row storage.
 * @param root Investment root bytes.
 * @param itemDefinitionCount Number of rows in the installed item index table.
 * @return True when every tag, class, bound, and item link validates and publishes.
 */
[[nodiscard]] bool build_collectibles(const reader::Source& source,
                                      Storage& storage,
                                      std::span<const std::byte> root,
                                      std::uint64_t itemDefinitionCount) noexcept;

/**
 * Walks the located item index table, then publishes every domain that depends on it.
 * @param source Package source.
 * @param storage Pass storage holding the located table blob.
 * @param table Located item index array.
 * @param rowCount Receives the dense item rows published.
 * @param reason Receives the step name when a stage fails.
 * @return True when the dense table and every dependent domain publish.
 */
[[nodiscard]] bool build_item_rows(const reader::Source& source,
                                   Storage& storage,
                                   const tables::Array& table,
                                   std::size_t& rowCount,
                                   const char*& reason) noexcept;

/** Reads and publishes every native material-requirement set from investment-root slot 96. */
[[nodiscard]] bool build_material_requirements(const reader::Source& source,
                                               Storage& storage,
                                               std::span<const std::byte> root,
                                               std::uint64_t itemDefinitionCount) noexcept;

} // namespace sunrise::client::content::items::packages
