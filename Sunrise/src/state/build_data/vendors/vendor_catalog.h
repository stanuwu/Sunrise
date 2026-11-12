#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::vendors {

/** Clears the index, every held definition, and every row bank. */
void clear() noexcept;

/**
 * Checks one complete vendor catalog in canonical order.
 * All five arrays check together, because a definition names its rows by range.
 * @param index Candidate index rows, dense and in ascending index order.
 * @param definitions Candidate definitions in ascending index order.
 * @param saleRows Candidate flat sale bank in definition then row order.
 * @param installedRows Candidate flat installed bank in definition then row order.
 * @param transferRules Candidate flat transfer bank in definition then row order.
 * @return True when every count, range, class, offset and ordering rule holds.
 */
[[nodiscard]] bool valid(std::span<const IndexEntry> index,
                         std::span<const Definition> definitions,
                         std::span<const SaleRow> saleRows,
                         std::span<const InstalledRow> installedRows,
                         std::span<const TransferRule> transferRules) noexcept;

/**
 * Replaces the complete vendor catalog in one step.
 * @param index Complete index rows.
 * @param definitions Complete definitions, which may be empty.
 * @param saleRows Complete flat sale bank.
 * @param installedRows Complete flat installed bank.
 * @param transferRules Complete flat transfer bank.
 * @return True when the catalog passes validation and fits fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const IndexEntry> index,
                           std::span<const Definition> definitions,
                           std::span<const SaleRow> saleRows,
                           std::span<const InstalledRow> installedRows,
                           std::span<const TransferRule> transferRules) noexcept;

/**
 * Finds one index row by the vendor definition hash.
 * @param definitionHash Vendor definition hash.
 * @param entry Receives the matching row.
 * @return True when exactly one index row carries the hash.
 */
[[nodiscard]] bool find_hash(std::uint32_t definitionHash, IndexEntry& entry) noexcept;

/**
 * Reads one index row by its position, which is the index the wire carries.
 * @param index Index row position.
 * @param entry Receives the row.
 * @return True when the table holds that row.
 */
[[nodiscard]] bool find_index(std::uint16_t index, IndexEntry& entry) noexcept;

/**
 * Finds one held definition by the vendor definition hash.
 * @param definitionHash Vendor definition hash.
 * @param definition Receives the matching definition.
 * @return True when a definition for that hash is held.
 */
[[nodiscard]] bool find(std::uint32_t definitionHash, Definition& definition) noexcept;

/**
 * Copies the sale rows one definition owns, in row order.
 * @param definition Definition whose range is copied.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold the whole range.
 */
[[nodiscard]] bool
sale_rows(const Definition& definition, std::span<SaleRow> output, std::size_t& count) noexcept;

/**
 * Reads one sale row of one definition, under the catalog lock.
 * @param definition Definition whose range is read.
 * @param row Row ordinal inside that definition.
 * @param output Receives the row, or a cleared row when the definition does not own it.
 * @return True when the definition owns that row.
 */
[[nodiscard]] bool
sale_row(const Definition& definition, std::size_t row, SaleRow& output) noexcept;

/**
 * Copies the installed rows one definition owns, in row order.
 * @param definition Definition whose range is copied.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold the whole range.
 */
[[nodiscard]] bool installed_rows(const Definition& definition,
                                  std::span<InstalledRow> output,
                                  std::size_t& count) noexcept;

/**
 * Reads one installed row of one definition, under the catalog lock.
 * @param definition Definition whose range is read.
 * @param row Row ordinal inside that definition.
 * @param output Receives the row, or a cleared row when the definition does not own it.
 * @return True when the definition owns that row.
 */
[[nodiscard]] bool
installed_row(const Definition& definition, std::size_t row, InstalledRow& output) noexcept;

/**
 * Reads one transfer rule of one definition, under the catalog lock.
 * @param definition Definition whose range is read.
 * @param row Rule ordinal inside that definition.
 * @param output Receives the rule, or a cleared rule when the definition does not own it.
 * @return True when the definition owns that rule.
 */
[[nodiscard]] bool
transfer_rule(const Definition& definition, std::size_t row, TransferRule& output) noexcept;

/**
 * Copies every index row in ascending index order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_index(std::span<IndexEntry> output, std::size_t& count) noexcept;

/**
 * Copies every held definition in ascending index order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_definitions(std::span<Definition> output, std::size_t& count) noexcept;

/**
 * Copies the whole flat sale bank.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_sale_rows(std::span<SaleRow> output, std::size_t& count) noexcept;

/**
 * Copies the whole flat installed bank.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_installed_rows(std::span<InstalledRow> output,
                                           std::size_t& count) noexcept;

/**
 * Copies the whole flat transfer bank.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot_transfer_rules(std::span<TransferRule> output,
                                           std::size_t& count) noexcept;

/** @return The index row count, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/** @return The held definition count, read under the lock. */
[[nodiscard]] std::size_t definition_count() noexcept;

/** @return The flat sale bank row count, read under the lock. */
[[nodiscard]] std::size_t sale_row_count() noexcept;

/** @return The flat installed bank row count, read under the lock. */
[[nodiscard]] std::size_t installed_row_count() noexcept;

} // namespace sunrise::state::build_data::vendors
