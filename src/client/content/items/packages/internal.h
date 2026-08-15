#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../../../core/filesystem/path.h"
#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/abilities/definition.h"
#include "../../../../state/build_data/constants/definition.h"
#include "../../../../state/build_data/items/details/definition.h"
#include "../../../../state/build_data/items/item_catalog.h"
#include "../../../../state/build_data/progressions/definition.h"

namespace sunrise::client::content::items::packages {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Configured equipment rows plus every plug they socket. */
inline constexpr std::size_t kDetailCapacity =
    state::build_data::items::details::kDefinitionCapacity;

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
    std::array<std::uint16_t, kDetailCapacity> requested{};
    std::array<state::build_data::items::details::Definition, kDetailCapacity> details{};
    AuthoredHashes authoredHashes{};
    std::vector<std::byte> abilityTable{};
    std::vector<std::byte> abilityPool{};
    std::array<state::build_data::abilities::Definition,
               state::build_data::abilities::kDefinitionCapacity>
        abilityRows{};
    std::array<state::build_data::progressions::Definition,
               state::build_data::progressions::kDefinitionCapacity>
        progressionRows{};
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
 * Adds one definition index to the requested set.
 * @param definitionIndex Native item index.
 * @param requested Requested-set storage.
 * @param count Used entries, advanced on success.
 * @return True when the index fits.
 */
[[nodiscard]] bool request(std::uint16_t definitionIndex,
                           std::span<std::uint16_t> requested,
                           std::size_t& count) noexcept;

/**
 * Adds every socket lane's initial plug to the requested set.
 * A lane the authored loadout leaves unset falls back to this plug, so its detail must exist.
 * @param row Item row already read from its definition blob.
 * @param requested Requested-set storage.
 * @param count Used entries, advanced per added lane.
 * @return True when every lane fits.
 */
[[nodiscard]] bool append_initial_plugs(const tables::items::Row& row,
                                        std::span<std::uint16_t> requested,
                                        std::size_t& count) noexcept;

/** @param requested Requested-set storage. @param count Sorted and deduplicated in place. */
void compact_requested(std::span<std::uint16_t> requested, std::size_t& count) noexcept;

/**
 * Reads one requested definition and turns it into its cached detail form.
 * @param source Located item index table and package reader state.
 * @param definitionIndex Native item index.
 * @param detail Receives the cached detail.
 * @return True when the row is found and its definition blob reads.
 */
[[nodiscard]] bool build_detail(const DetailSource& source,
                                std::uint16_t definitionIndex,
                                state::build_data::items::details::Definition& detail) noexcept;

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
[[nodiscard]] bool
build_character_abilities(const reader::Source& source,
                          reader::Scratch& scratch,
                          std::span<const std::byte> root,
                          std::vector<std::byte>& table,
                          std::vector<std::byte>& definition,
                          std::vector<std::byte>& blob,
                          std::span<state::build_data::abilities::Definition> output,
                          std::size_t& count) noexcept;

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

/** @param count Detail rows the pass built, covering equipped items and every plug they socket. */
void report_detail_count(std::size_t count) noexcept;

/** Reports the pass outcome once. @param published Rows published, or zero on failure. */
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

} // namespace sunrise::client::content::items::packages
