#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/vendors/definition.h"
#include "layout.h"
#include "vendor_build.h"

namespace sunrise::client::content::vendors {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::vendors;

/** Every extracted row, kept off the caller stack. */
struct Storage {
    std::vector<std::byte> blob{};
    std::array<domain::IndexEntry, domain::kIndexCapacity> index{};
    std::array<domain::Definition, domain::kDefinitionCapacity> definitions{};
    std::array<domain::SaleRow, domain::kSaleRowCapacity> saleRows{};
    std::array<domain::InstalledRow, domain::kInstalledRowCapacity> installedRows{};
    std::size_t indexCount{};
    std::size_t definitionCount{};
    std::size_t saleRowCount{};
    std::size_t installedRowCount{};
    /** Sale rows whose price depends on an expression, kept but never charged. */
    std::size_t conditionalRows{};
    /** Sale rows whose cost array did not read, kept but never charged. */
    std::size_t unreadableRows{};
};

/** One array a definition or a sale row declares, reduced to what the catalog stores. */
struct ArrayView {
    std::uint32_t base{};
    std::uint32_t classId{};
    std::uint16_t count{};
};

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

/**
 * Reads one array descriptor and bounds it against the blob holding it.
 * The raw count is read first, because the shared resolver reports absent and corrupt alike.
 * @param blob Whole blob owning the descriptor.
 * @param descriptor Descriptor offset.
 * @param stride One row's size.
 * @param output Receives the array, or an absent array.
 * @return True when the array is absent, or resolves and ends inside the blob.
 */
[[nodiscard]] bool read_array(std::span<const std::byte> blob,
                              std::size_t descriptor,
                              std::size_t stride,
                              ArrayView& output) noexcept {
    /** Row counts are stored as unsigned 16-bit values. */
    constexpr std::uint64_t kMaximumCount = (std::numeric_limits<std::uint16_t>::max)();
    output = {};
    std::uint64_t declared = 0;
    if (!read(blob, descriptor, declared)) {
        return false;
    }
    if (declared == 0) {
        return true;
    }
    tables::Array array{};
    if (!tables::find_array_at(blob, descriptor, array) || array.count > kMaximumCount) {
        return false;
    }
    const std::uint64_t end = array.dataOffset + (array.count * stride);
    if (end > blob.size()) {
        return false;
    }
    output = {static_cast<std::uint32_t>(array.dataOffset),
              array.elementClass,
              static_cast<std::uint16_t>(array.count)};
    return true;
}

/**
 * Reads what one sale row charges: every entry of its cost array, and whether those entries are
 * the price. An entry is static only when both of its expression arrays are empty and its
 * trailing word is the plain value. One carrying an expression, or another word, makes the row
 * conditional: its price then depends on state this build does not evaluate, so the entries are
 * kept for the record and the row is never charged.
 * @param blob Whole definition blob.
 * @param at Sale row offset inside the blob.
 * @param value Receives the entries and the row's price state.
 * @return True when the array is absent, or resolves, fits, and ends inside the blob. Otherwise
 *         the row is left unreadable, with no entries.
 */
[[nodiscard]] bool
read_sale_cost(std::span<const std::byte> blob, std::size_t at, domain::SaleRow& value) noexcept {
    value.costs = {};
    value.costCount = 0;
    value.priceState = domain::PriceState::unreadable;
    ArrayView cost{};
    if (!read_array(blob, at + kSaleCostArrayDescriptor, domain::kSaleCostRowStride, cost)) {
        return false;
    }
    if (cost.count != 0
        && (cost.classId != domain::kSaleCostRowClass || cost.count > value.costs.size())) {
        return false;
    }
    bool conditional = false;
    for (std::size_t entry = 0; entry < cost.count; ++entry) {
        const std::size_t entryAt = cost.base + (entry * domain::kSaleCostRowStride);
        domain::SaleCost& output = value.costs[entry];
        std::uint64_t firstProgram = 0;
        std::uint64_t secondProgram = 0;
        std::uint32_t word = 0;
        if (!read(blob, entryAt + kSaleCostItemIndexOffset, output.itemIndex)
            || !read(blob, entryAt + kSaleCostQuantityOffset, output.quantity)
            || !read(blob, entryAt + kSaleCostFirstProgramDescriptor, firstProgram)
            || !read(blob, entryAt + kSaleCostSecondProgramDescriptor, secondProgram)
            || !read(blob, entryAt + kSaleCostWordOffset, word)) {
            value.costs = {};
            return false;
        }
        conditional = conditional || firstProgram != 0 || secondProgram != 0
                      || word != domain::kPlainCostWord;
    }
    value.costCount = static_cast<std::uint8_t>(cost.count);
    value.priceState = conditional ? domain::PriceState::conditional : domain::PriceState::plain;
    return true;
}

/**
 * Reads the whole installed vendor index.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage receiving the index rows.
 * @return True when the index blob reads and every row fits.
 */
[[nodiscard]] bool
read_index(const reader::Source& source, reader::Scratch& scratch, Storage& storage) noexcept {
    std::uint32_t classId = 0;
    tables::Array array{};
    if (!reader::read_tag(source, scratch, kIndexRootTag, storage.blob, classId)
        || classId != domain::kIndexWrapperClass) {
        return false;
    }
    const std::span<const std::byte> blob{storage.blob};
    if (!tables::find_array_at(blob, tables::kTableArrayDescriptor, array)
        || array.elementClass != domain::kIndexRowClass || array.count > domain::kIndexCapacity) {
        return false;
    }
    for (std::uint64_t row = 0; row < array.count; ++row) {
        tables::IndexRow entry{};
        if (!tables::index_row(blob, array, row, entry)) {
            return false;
        }
        storage.index[storage.indexCount] = {
            entry.definitionHash, entry.targetTag, static_cast<std::uint16_t>(row)};
        ++storage.indexCount;
    }
    return storage.indexCount != 0;
}

/**
 * Reads every sale row of one definition into the flat bank.
 * A row whose cost array does not read costs that row its price, never the vendor: a purchase
 * names the row by ordinal, so the row stays in place and refuses to be bought.
 * @param blob Whole definition blob.
 * @param definition Definition whose sale array was already resolved.
 * @param storage Pass storage receiving the rows.
 * @return True when every row is inside the blob and the bank holds them all.
 */
[[nodiscard]] bool read_sale_rows(std::span<const std::byte> blob,
                                  const domain::Definition& definition,
                                  Storage& storage) noexcept {
    if (definition.saleCount > domain::kSaleRowCapacity - storage.saleRowCount) {
        return false;
    }
    for (std::size_t row = 0; row < definition.saleCount; ++row) {
        const std::size_t at = definition.saleRowBase + (row * domain::kSaleRowStride);
        domain::SaleRow& value = storage.saleRows[storage.saleRowCount + row];
        value = {};
        if (!read(blob, at + kSaleItemIndexOffset, value.itemIndex)
            || !read(blob, at + kSaleSecondaryItemOffset, value.secondaryItemIndex)
            || !read(blob, at + kSaleCategoryIndexOffset, value.categoryIndex)) {
            return false;
        }
        if (!read_sale_cost(blob, at, value)) {
            ++storage.unreadableRows;
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=build_data stage=vendors result=unreadable_cost hash=0x%08X "
                              "row=%zu",
                              definition.definitionHash,
                              row);
        } else if (value.priceState == domain::PriceState::conditional) {
            ++storage.conditionalRows;
        }
    }
    storage.saleRowCount += definition.saleCount;
    return true;
}

/**
 * Reads the definition hash of every category row of one definition into the flat bank.
 * @param blob Whole definition blob.
 * @param definition Definition whose installed array was already resolved.
 * @param storage Pass storage receiving the rows.
 * @return True when every row is inside the blob and the bank holds them all.
 */
[[nodiscard]] bool read_installed_rows(std::span<const std::byte> blob,
                                       const domain::Definition& definition,
                                       Storage& storage) noexcept {
    if (definition.installedCount > domain::kInstalledRowCapacity - storage.installedRowCount) {
        return false;
    }
    for (std::size_t row = 0; row < definition.installedCount; ++row) {
        const std::size_t at = definition.installedRowBase + (row * domain::kInstalledRowStride);
        domain::InstalledRow& value = storage.installedRows[storage.installedRowCount + row];
        value = {};
        if (!read(blob, at + kInstalledRowHashOffset, value.definitionHash)) {
            return false;
        }
    }
    storage.installedRowCount += definition.installedCount;
    return true;
}

/**
 * Reads one vendor definition and both of its row arrays.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param entry Index row naming the definition.
 * @param storage Pass storage receiving the definition and its rows.
 * @return True when the definition blob reads and every array ends inside it.
 */
[[nodiscard]] bool read_definition(const reader::Source& source,
                                   reader::Scratch& scratch,
                                   const domain::IndexEntry& entry,
                                   Storage& storage) noexcept {
    /** Definition sizes are stored as unsigned 32-bit values. */
    constexpr std::size_t kMaximumSize = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t classId = 0;
    if (storage.definitionCount == domain::kDefinitionCapacity
        || !reader::read_tag(source, scratch, entry.definitionTag, storage.blob, classId)
        || classId != domain::kDefinitionClass || storage.blob.size() > kMaximumSize) {
        return false;
    }
    const std::span<const std::byte> blob{storage.blob};
    ArrayView installed{};
    ArrayView sale{};
    ArrayView third{};
    if (!read_array(blob, kInstalledArrayDescriptor, domain::kInstalledRowStride, installed)
        || !read_array(blob, kSaleArrayDescriptor, domain::kSaleRowStride, sale)
        || !read_array(blob, kThirdArrayDescriptor, domain::kThirdRowStride, third)) {
        return false;
    }
    domain::Definition definition{};
    definition.definitionHash = entry.definitionHash;
    definition.definitionTag = entry.definitionTag;
    definition.definitionClass = classId;
    definition.definitionSize = static_cast<std::uint32_t>(blob.size());
    definition.index = entry.index;
    definition.installedRowBase = installed.base;
    definition.installedRowClass = installed.classId;
    definition.installedCount = installed.count;
    definition.saleRowBase = sale.base;
    definition.saleRowClass = sale.classId;
    definition.saleCount = sale.count;
    definition.thirdRowBase = third.base;
    definition.thirdRowClass = third.classId;
    definition.thirdCount = third.count;
    definition.saleRowOffset = static_cast<std::uint32_t>(storage.saleRowCount);
    definition.installedRowOffset = static_cast<std::uint32_t>(storage.installedRowCount);
    // A skipped definition must leave both banks exactly as it found them; an orphan sale row
    // shifts the next definition's offset and `valid()` then rejects the whole set.
    const std::size_t saleRowsBefore = storage.saleRowCount;
    const std::size_t installedRowsBefore = storage.installedRowCount;
    if (!read(blob, kResetIntervalOffset, definition.resetIntervalRaw)
        || !read(blob, kResetPhaseOffset, definition.resetPhaseRaw)
        || !read_sale_rows(blob, definition, storage)
        || !read_installed_rows(blob, definition, storage)) {
        storage.saleRowCount = saleRowsBefore;
        storage.installedRowCount = installedRowsBefore;
        return false;
    }
    storage.definitions[storage.definitionCount] = definition;
    ++storage.definitionCount;
    return true;
}

/**
 * Reports the pass so a boot with no vendor catalog says which step lost the rows.
 * @param storage Pass storage holding every count.
 * @param skipped Requested definitions that could not be read or could not fit.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage, std::size_t skipped, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=vendors index=%zu definitions=%zu "
                                      "sale=%zu conditional=%zu unreadable=%zu installed=%zu "
                                      "skipped=%zu result=%s",
                                      storage.indexCount,
                                      storage.definitionCount,
                                      storage.saleRowCount,
                                      storage.conditionalRows,
                                      storage.unreadableRows,
                                      storage.installedRowCount,
                                      skipped,
                                      result);
    if (written > 0) {
        const bool clean = storage.indexCount != 0 && skipped == 0 && storage.unreadableRows == 0;
        core::log::write(core::log::Channel::state,
                         clean ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Extracts and publishes the vendor catalog from the installed packages. */
bool build(const reader::Source& source, reader::Scratch& scratch) noexcept {
    if (state::build_data::vendor_catalog_ready()) {
        return true;
    }
    static Storage storage{};
    storage = {};
    if (!read_index(source, scratch, storage)) {
        report(storage, 0, "index");
        return false;
    }
    // Walk the index in order: the catalog requires ascending definition order. A definition that
    // will not read or will not fit costs that vendor alone, never the whole pass.
    std::size_t skipped = 0;
    for (std::size_t row = 0; row < storage.indexCount; ++row) {
        const domain::IndexEntry entry = storage.index[row];
        if (read_definition(source, scratch, entry, storage)) {
            continue;
        }
        ++skipped;
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=build_data stage=vendors result=skip hash=0x%08X row=%zu "
                          "definitions=%zu sale=%zu",
                          entry.definitionHash,
                          row,
                          storage.definitionCount,
                          storage.saleRowCount);
    }
    const bool published = state::build_data::publish_vendor_catalog(
        std::span(storage.index).first(storage.indexCount),
        std::span(storage.definitions).first(storage.definitionCount),
        std::span(storage.saleRows).first(storage.saleRowCount),
        std::span(storage.installedRows).first(storage.installedRowCount));
    report(storage, skipped, published ? "ok" : "publish");
    return published;
}

} // namespace sunrise::client::content::vendors
