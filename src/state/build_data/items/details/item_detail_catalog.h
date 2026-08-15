#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::items::details {

/** Clears every generated configured item detail. */
void clear() noexcept;

/**
 * Checks one configured item-detail table, without any cross-domain bounds.
 * @param definitions Candidate installed-build item details.
 * @return True when every record is valid and its native index is unique.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated configured item-detail table in one step.
 * @param definitions Complete installed-build item details.
 * @return True when all records pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Finds one configured item detail by native definition index.
 * @param definitionIndex Native installed-build item index.
 * @param definition Receives the matching configured item detail.
 * @return True when the table holds that index.
 */
[[nodiscard]] bool find(std::uint16_t definitionIndex, Definition& definition) noexcept;

/**
 * Copies every detail in publication order.
 * @param output Caller-owned fixed detail storage.
 * @param count Receives the copied detail count, or zero when output is too small.
 * @return True when output can hold the whole table.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of configured item details, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::items::details
