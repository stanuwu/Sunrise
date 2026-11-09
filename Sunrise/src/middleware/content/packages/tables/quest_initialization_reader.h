#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../state/build_data/items/quest_initialization.h"
#include "../../../../state/build_data/pursuits/definition.h"

namespace sunrise::middleware::content::packages::tables::items {

/** The all-one item index cannot name a quest-set owner. */
inline constexpr std::uint16_t kUnavailableQuestParent = 0xFFFFU;

/**
 * A slot must match once across all maps, including maps with no supported save bank.
 * @param valueMap Blob containing all four unlock value maps.
 * @param slot Value slot to map.
 * @param row Receives the saved bank row; use only on success.
 * @param scope Receives the bank that row belongs to; use only on success.
 * @return False when the slot matches no row, several rows, or a map with no saved bank.
 */
[[nodiscard]] bool
map_value_slot(std::span<const std::byte> valueMap,
               std::uint16_t slot,
               std::uint16_t& row,
               state::build_data::items::QuestInitialization::Scope& scope) noexcept;

/**
 * Only an objective-bearing pursuit can name a quest-set owner.
 * @param definition Item definition bytes, including its nested blocks.
 * @return The owner's item-table index, or kUnavailableQuestParent on rejection.
 */
[[nodiscard]] std::uint16_t quest_parent(std::span<const std::byte> definition) noexcept;

/**
 * Only a unique first member with one supported save-bank mapping may start a quest.
 * @param definition Pursuit item being acquired.
 * @param itemIndex Pursuit's item-table index.
 * @param parent Set-owner bytes selected by quest_parent; may be definition itself.
 * @param itemCount Exclusive bound for item-table indices.
 * @param valueMap Blob containing all four unlock value maps.
 * @return The first-step value and bank row, or an empty plan for unsupported content.
 */
[[nodiscard]] state::build_data::items::QuestInitialization
read_quest_initialization(std::span<const std::byte> definition,
                          std::uint16_t itemIndex,
                          std::span<const std::byte> parent,
                          std::size_t itemCount,
                          std::span<const std::byte> valueMap) noexcept;

/**
 * Members must name distinct items and step values, since the saved value selects the step.
 * @param definition Pursuit item.
 * @param itemIndex Pursuit's item-table index.
 * @param parent Set-owner bytes selected by quest_parent; may be definition itself.
 * @param itemCount Exclusive bound for item-table indices.
 * @param valueMap Blob containing all four unlock value maps.
 * @return The item's step and the member after it; no membership when no set names the item or
 *         the item is its set's only member; unsupported for any other set.
 */
[[nodiscard]] state::build_data::pursuits::QuestStep
read_quest_step(std::span<const std::byte> definition,
                std::uint16_t itemIndex,
                std::span<const std::byte> parent,
                std::size_t itemCount,
                std::span<const std::byte> valueMap) noexcept;

} // namespace sunrise::middleware::content::packages::tables::items
