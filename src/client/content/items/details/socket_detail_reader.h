#pragma once

#include <cstddef>
#include <cstdint>

#include "configured_item_detail_extractor.h"

namespace sunrise::client::content::items::details::socket_reader {

namespace build_details = state::build_data::items::details;

/**
 * Reads the optional ordinary-socket block and its initial plug indices.
 * @param source Installed item-detail source.
 * @param definition Loaded item definition address.
 * @param itemRowCount Current dense item-table row bound.
 * @param output Detail receiving block state, count, and initial plugs.
 * @return True when the optional array is absent or fully good.
 */
[[nodiscard]] bool read_ordinary(const investment::Source& source,
                                 std::uintptr_t definition,
                                 std::size_t itemRowCount,
                                 build_details::Definition& output) noexcept;

/**
 * Reads the optional socket-entry-list row selection.
 * @param source Installed item-detail source.
 * @param definition Loaded item definition address.
 * @param rowCount Current socket-entry-list table bound.
 * @param output Detail receiving row 0 or the checked pick.
 * @return True when the optional block is absent or picks an in-range row.
 */
[[nodiscard]] bool read_list_index(const investment::Source& source,
                                   std::uintptr_t definition,
                                   std::size_t rowCount,
                                   build_details::Definition& output) noexcept;

} // namespace sunrise::client::content::items::details::socket_reader
