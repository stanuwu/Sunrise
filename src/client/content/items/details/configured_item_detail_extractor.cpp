#include "configured_item_detail_extractor.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

#include "../../../../state/build_data/items/item_catalog.h"
#include "../../../../state/build_data/socket_entry_lists/definition.h"
#include "../../investment/layout.h"
#include "../layout.h"
#include "definition_detail_reader.h"
#include "memory.h"

namespace sunrise::client::content::items::details {
namespace {

namespace build_details = state::build_data::items::details;
namespace build_items = state::build_data::items;
namespace build_socket_lists = state::build_data::socket_entry_lists;
namespace dense_layout = sunrise::client::content::items::layout;
namespace investment_layout = sunrise::client::content::investment::layout;

/**
 * Finds the installed item table once and checks its starting row count.
 * @param source Installed investment globals and bounded memory source.
 * @param table Receives the table address, first-row address, and starting count.
 * @return True when all three handles are found and the dense count fits native State.
 */
[[nodiscard]] bool resolve_table(const investment::Source& source, reader::Table& table) noexcept {
    std::uintptr_t globals = 0;
    std::uintptr_t root = 0;
    if (!handles::resolve(source.handles, source.investmentGlobalsTag, globals)
        || !memory::resolve_member(
            source,
            globals,
            offsetof(investment_layout::InvestmentGlobals, investmentRootTag),
            root)
        || !memory::resolve_member(source,
                                   root,
                                   offsetof(investment_layout::InvestmentRoot, itemTableTag),
                                   table.tableAddress)) {
        return false;
    }

    std::uint64_t rowCount = 0;
    if (!memory::read_member(
            source, table.tableAddress, offsetof(dense_layout::ItemTable, rowCount), rowCount)
        || rowCount == 0 || rowCount > static_cast<std::uint64_t>(build_items::kDefinitionCapacity)
        || !memory::member_address(
            table.tableAddress, offsetof(dense_layout::ItemTable, firstRow), table.rowsAddress)) {
        return false;
    }
    table.rowCount = static_cast<std::size_t>(rowCount);
    return true;
}

/**
 * Confirms the dense table count still matches the value used for every row bound.
 * @param source Installed item-detail source.
 * @param table Found table and its starting row count.
 * @return True when a last full count read is unchanged.
 */
[[nodiscard]] bool stable_count(const investment::Source& source,
                                const reader::Table& table) noexcept {
    std::uint64_t finalCount = 0;
    return memory::read_member(
               source, table.tableAddress, offsetof(dense_layout::ItemTable, rowCount), finalCount)
           && finalCount == static_cast<std::uint64_t>(table.rowCount);
}

} // namespace

/** Extracts full native details for configured definition indices. */
bool extract(const investment::Source& source,
             std::span<const std::uint16_t> requestedDefinitionIndices,
             std::size_t socketEntryListRowCount,
             std::span<build_details::Definition> output,
             std::size_t& count) noexcept {
    count = 0;
    if (requestedDefinitionIndices.empty()) {
        return true;
    }
    if (requestedDefinitionIndices.size() > build_details::kDefinitionCapacity
        || requestedDefinitionIndices.size() > output.size() || socketEntryListRowCount == 0
        || socketEntryListRowCount > build_socket_lists::kDefinitionCapacity) {
        return false;
    }

    reader::Table table{};
    if (!resolve_table(source, table)) {
        return false;
    }

    std::bitset<build_items::kDefinitionCapacity> seen{};
    std::array<build_details::Definition, build_details::kDefinitionCapacity> staged{};
    for (std::size_t position = 0; position < requestedDefinitionIndices.size(); ++position) {
        const std::uint16_t definitionIndex = requestedDefinitionIndices[position];
        if (static_cast<std::size_t>(definitionIndex) >= table.rowCount
            || seen.test(definitionIndex)) {
            return false;
        }
        seen.set(definitionIndex);
        if (!reader::read_definition(
                source, table, definitionIndex, socketEntryListRowCount, staged[position])) {
            return false;
        }
    }

    // The final count read is the commit gate for the staged caller snapshot.
    if (!stable_count(source, table)) {
        return false;
    }
    std::copy_n(staged.begin(), requestedDefinitionIndices.size(), output.begin());
    count = requestedDefinitionIndices.size();
    return true;
}

} // namespace sunrise::client::content::items::details
