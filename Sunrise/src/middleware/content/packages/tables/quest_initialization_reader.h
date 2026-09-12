#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../state/build_data/items/quest_initialization.h"

namespace sunrise::middleware::content::packages::tables::items {

/** The all-one item index cannot name a quest-set owner. */
inline constexpr std::uint16_t kUnavailableQuestParent = 0xFFFFU;

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

} // namespace sunrise::middleware::content::packages::tables::items
