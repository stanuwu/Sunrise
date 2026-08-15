#pragma once

#include <cstddef>
#include <cstdint>

#include "configured_item_detail_extractor.h"

namespace sunrise::client::content::items::details::reader {

namespace build_details = state::build_data::items::details;

/** Resolved dense item table held only for one bounded extraction call. */
struct Table {
    std::uintptr_t tableAddress{};
    std::uintptr_t rowsAddress{};
    std::size_t rowCount{};
};

/**
 * Reads and checks one requested native item definition.
 * @param source Installed item-detail source.
 * @param table Stable-call table address, row start, and starting count.
 * @param definitionIndex Requested native row index.
 * @param socketEntryListRowCount Current socket-entry-list table bound.
 * @param output Receives a full detail only on success.
 * @return True when the target and every needed block pass their checks.
 */
[[nodiscard]] bool read_definition(const investment::Source& source,
                                   const Table& table,
                                   std::uint16_t definitionIndex,
                                   std::size_t socketEntryListRowCount,
                                   build_details::Definition& output) noexcept;

} // namespace sunrise::client::content::items::details::reader
