#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../state/build_data/items/details/definition.h"
#include "../../investment/source.h"

namespace sunrise::client::content::items::details {

/**
 * Extracts full native details for configured definition indices.
 * @param source Installed investment globals and bounded memory source.
 * @param requestedDefinitionIndices Unique native indices in caller order.
 * @param socketEntryListRowCount Current socket-entry-list table bound.
 * @param output Caller storage left unchanged on failure.
 * @param count Receives the detail count, or zero on failure.
 * @return True when the item-table count stays stable and every detail passes its checks.
 */
[[nodiscard]] bool extract(const investment::Source& source,
                           std::span<const std::uint16_t> requestedDefinitionIndices,
                           std::size_t socketEntryListRowCount,
                           std::span<state::build_data::items::details::Definition> output,
                           std::size_t& count) noexcept;

} // namespace sunrise::client::content::items::details
