#include "vendor_catalog.h"

#include <algorithm>
#include <shared_mutex>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::vendors {
namespace {

// One lock covers all four tables. A definition names its rows by range, so a reader must
// never see one table replaced and another not.
core::threading::SrwLock g_lock;
Table<IndexEntry, kIndexCapacity> g_index;
Table<Definition, kDefinitionCapacity> g_definitions;
Table<SaleRow, kSaleRowCapacity> g_saleRows;
Table<InstalledRow, kInstalledRowCapacity> g_installedRows;

/**
 * Checks one array a definition declares against the definition blob it sits in.
 * An absent array declares no base and no class, so a zero count is checked as absence.
 * @param count Declared row count.
 * @param base First row offset inside the definition blob.
 * @param classId Element class the array header carries.
 * @param stride One row's size.
 * @param expectedClass Required element class, or zero to accept any class.
 * @param definitionSize Definition blob size.
 * @return True when the array is absent, or ends inside the blob with the required class.
 */
[[nodiscard]] bool array_fits(std::uint16_t count,
                              std::uint32_t base,
                              std::uint32_t classId,
                              std::size_t stride,
                              std::uint32_t expectedClass,
                              std::uint32_t definitionSize) noexcept {
    if (count == 0) {
        return base == 0 && classId == 0;
    }
    if (expectedClass != 0 && classId != expectedClass) {
        return false;
    }
    const std::uint64_t end =
        static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(count) * stride;
    return end <= definitionSize;
}

/**
 * Checks one definition against the index row it is named by and its own blob bounds.
 * @param definition Candidate definition.
 * @param index Complete index rows.
 * @return True when the definition names a known index row and every array ends inside the blob.
 */
[[nodiscard]] bool canonical(const Definition& definition,
                             std::span<const IndexEntry> index) noexcept {
    if (definition.index >= index.size() || definition.definitionClass != kDefinitionClass
        || definition.definitionSize == 0
        || definition.definitionHash != index[definition.index].definitionHash
        || definition.definitionTag != index[definition.index].definitionTag) {
        return false;
    }
    return array_fits(definition.installedCount,
                      definition.installedRowBase,
                      definition.installedRowClass,
                      kInstalledRowStride,
                      kInstalledRowClass,
                      definition.definitionSize)
           && array_fits(definition.saleCount,
                         definition.saleRowBase,
                         definition.saleRowClass,
                         kSaleRowStride,
                         kSaleRowClass,
                         definition.definitionSize)
           // The third array has no closed element class, so only its bounds are checked.
           && array_fits(definition.thirdCount,
                         definition.thirdRowBase,
                         definition.thirdRowClass,
                         kThirdRowStride,
                         0,
                         definition.definitionSize);
}

/**
 * Checks the sale rows one definition owns.
 * @param definition Owning definition.
 * @param saleRows Complete flat sale bank.
 * @return True when every row names a category of this definition, or none at all, and its cost
 *         entries fit their state: an unreadable row has none.
 */
[[nodiscard]] bool canonical_sale_rows(const Definition& definition,
                                       std::span<const SaleRow> saleRows) noexcept {
    for (std::size_t row = 0; row < definition.saleCount; ++row) {
        const SaleRow& value = saleRows[definition.saleRowOffset + row];
        // Row +100 is bounded by the category count before any reader strides with it.
        const bool selects =
            value.categoryIndex == kAbsentCategoryIndex
            || (value.categoryIndex >= 0 && value.categoryIndex < definition.installedCount);
        const bool priced =
            value.costCount <= value.costs.size()
            && (value.priceState == PriceState::plain || value.priceState == PriceState::conditional
                || (value.priceState == PriceState::unreadable && value.costCount == 0));
        if (!selects || !priced) {
            return false;
        }
    }
    return true;
}

/**
 * Copies one contiguous bank range into caller storage.
 * @tparam Row Bank row type.
 * @param bank Whole published bank.
 * @param offset First row of the range.
 * @param rows Rows in the range.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when the range does not fit.
 * @return True when output can hold the whole range.
 */
template <typename Row>
[[nodiscard]] bool copy_range(std::span<const Row> bank,
                              std::size_t offset,
                              std::size_t rows,
                              std::span<Row> output,
                              std::size_t& count) noexcept {
    count = 0;
    // A definition copied before a replace can name a range the current bank no longer has.
    const bool fits =
        output.size() >= rows && offset <= bank.size() && rows <= bank.size() - offset;
    if (fits) {
        std::copy_n(bank.begin() + static_cast<std::ptrdiff_t>(offset), rows, output.begin());
        count = rows;
    }
    return fits;
}

} // namespace

/** Clears the index, every held definition, and both row banks under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_index.clear();
    g_definitions.clear();
    g_saleRows.clear();
    g_installedRows.clear();
}

/** Checks one complete vendor catalog in canonical order. */
bool valid(std::span<const IndexEntry> index,
           std::span<const Definition> definitions,
           std::span<const SaleRow> saleRows,
           std::span<const InstalledRow> installedRows) noexcept {
    if (index.empty() || index.size() > kIndexCapacity || definitions.size() > kDefinitionCapacity
        || saleRows.size() > kSaleRowCapacity || installedRows.size() > kInstalledRowCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < index.size(); ++row) {
        if (index[row].index != row) {
            return false;
        }
    }
    std::size_t saleOffset = 0;
    std::size_t installedOffset = 0;
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        const Definition& definition = definitions[row];
        if (!canonical(definition, index) || definition.saleRowOffset != saleOffset
            || definition.installedRowOffset != installedOffset
            || definition.saleCount > saleRows.size() - saleOffset
            || definition.installedCount > installedRows.size() - installedOffset
            || (row != 0 && definitions[row - 1].index >= definition.index)
            || !canonical_sale_rows(definition, saleRows)) {
            return false;
        }
        saleOffset += definition.saleCount;
        installedOffset += definition.installedCount;
    }
    return saleOffset == saleRows.size() && installedOffset == installedRows.size();
}

/** Replaces the complete vendor catalog in one step. */
bool replace(std::span<const IndexEntry> index,
             std::span<const Definition> definitions,
             std::span<const SaleRow> saleRows,
             std::span<const InstalledRow> installedRows) noexcept {
    if (!valid(index, definitions, saleRows, installedRows)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    // All four run with no short-circuit, so the set cannot be left half replaced. Capacity is
    // the only reason one can refuse, and valid() already checked it.
    const bool storedIndex = g_index.replace(index);
    const bool storedDefinitions = g_definitions.replace(definitions);
    const bool storedSaleRows = g_saleRows.replace(saleRows);
    const bool storedInstalledRows = g_installedRows.replace(installedRows);
    return storedIndex && storedDefinitions && storedSaleRows && storedInstalledRows;
}

/** Finds one index row by the vendor definition hash. */
bool find_hash(std::uint32_t definitionHash, IndexEntry& entry) noexcept {
    entry = {};
    const std::shared_lock guard(g_lock);
    const std::span<const IndexEntry> rows = g_index.rows();
    const auto found =
        std::find_if(rows.begin(), rows.end(), [definitionHash](const IndexEntry& row) {
            return row.definitionHash == definitionHash;
        });
    // A hash carried by two rows names no single vendor, so neither row is returned.
    const bool unique =
        found != rows.end()
        && std::find_if(found + 1, rows.end(), [definitionHash](const IndexEntry& row) {
               return row.definitionHash == definitionHash;
           }) == rows.end();
    if (unique) {
        entry = *found;
    }
    return unique;
}

/** Reads one index row by its position. */
bool find_index(std::uint16_t index, IndexEntry& entry) noexcept {
    entry = {};
    const std::shared_lock guard(g_lock);
    const std::span<const IndexEntry> rows = g_index.rows();
    const bool present = index < rows.size();
    if (present) {
        entry = rows[index];
    }
    return present;
}

/** Finds one held definition by the vendor definition hash. */
bool find(std::uint32_t definitionHash, Definition& definition) noexcept {
    definition = {};
    const std::shared_lock guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    const auto found =
        std::find_if(rows.begin(), rows.end(), [definitionHash](const Definition& row) {
            return row.definitionHash == definitionHash;
        });
    const bool present = found != rows.end();
    if (present) {
        definition = *found;
    }
    return present;
}

/** Copies the sale rows one definition owns. */
bool sale_rows(const Definition& definition,
               std::span<SaleRow> output,
               std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return copy_range(
        g_saleRows.rows(), definition.saleRowOffset, definition.saleCount, output, count);
}

/** Reads one sale row of one definition. */
bool sale_row(const Definition& definition, std::size_t row, SaleRow& output) noexcept {
    output = {};
    if (row >= definition.saleCount) {
        return false;
    }
    const std::shared_lock guard(g_lock);
    const auto bank = g_saleRows.rows();
    const std::size_t at = static_cast<std::size_t>(definition.saleRowOffset) + row;
    if (at >= bank.size()) {
        return false;
    }
    output = bank[at];
    return true;
}

/** Copies the installed rows one definition owns. */
bool installed_rows(const Definition& definition,
                    std::span<InstalledRow> output,
                    std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return copy_range(g_installedRows.rows(),
                      definition.installedRowOffset,
                      definition.installedCount,
                      output,
                      count);
}

/** Reads one installed row of one definition. */
bool installed_row(const Definition& definition, std::size_t row, InstalledRow& output) noexcept {
    output = {};
    if (row >= definition.installedCount) {
        return false;
    }
    const std::shared_lock guard(g_lock);
    const auto bank = g_installedRows.rows();
    const std::size_t at = static_cast<std::size_t>(definition.installedRowOffset) + row;
    if (at >= bank.size()) {
        return false;
    }
    output = bank[at];
    return true;
}

/** Copies every index row in ascending index order. */
bool snapshot_index(std::span<IndexEntry> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_index.snapshot(output, count);
}

/** Copies every held definition in ascending index order. */
bool snapshot_definitions(std::span<Definition> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** Copies the whole flat sale bank. */
bool snapshot_sale_rows(std::span<SaleRow> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_saleRows.snapshot(output, count);
}

/** Copies the whole flat installed bank. */
bool snapshot_installed_rows(std::span<InstalledRow> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_installedRows.snapshot(output, count);
}

/** @return The index row count, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_index.count();
}

/** @return The held definition count, read under the lock. */
std::size_t definition_count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.count();
}

/** @return The flat sale bank row count, read under the lock. */
std::size_t sale_row_count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_saleRows.count();
}

/** @return The flat installed bank row count, read under the lock. */
std::size_t installed_row_count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_installedRows.count();
}

} // namespace sunrise::state::build_data::vendors
