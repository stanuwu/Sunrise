#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../../../investment/investment.h"
#include "definition.h"

namespace sunrise::state::build_data::items::catalysts {

/** Clears all build-derived catalyst records without changing the configured completion policy. */
void clear() noexcept;

/** @param enabled True to complete released catalysts during item resolution. */
void set_completion_enabled(bool enabled) noexcept;

/** @return The current global default-completion policy. */
[[nodiscard]] bool completion_enabled() noexcept;

/**
 * @param definitions Complete candidate catalog in native item-index order.
 * @return True when all rows and unavailable fields are canonical.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * @param definitions Complete validated catalog in native item-index order.
 * @return True when the catalog fits fixed storage and replaces the old rows.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * @param itemDefinitionIndex Native weapon definition index.
 * @return Released state, safe skip state, or absence for the item.
 */
[[nodiscard]] Result resolve(std::uint16_t itemDefinitionIndex) noexcept;

/**
 * Resolves the item row that supplies a socketed catalyst's native perks and stat changes.
 * Direct catalyst plugs resolve to themselves. Legacy display plugs resolve to their linked
 * effect item. Unreleased, incomplete, and non-catalyst plugs stay unchanged.
 * @param itemDefinitionIndex Native weapon definition index.
 * @param socketLane Ordinary socket lane.
 * @param plugDefinitionIndex Socketed plug definition index.
 * @return Effective item definition index for perk and stat output.
 */
[[nodiscard]] std::uint16_t resolve_effect(std::uint16_t itemDefinitionIndex,
                                           std::uint8_t socketLane,
                                           std::uint16_t plugDefinitionIndex) noexcept;

/**
 * @param itemDefinitionIndex Native weapon definition index.
 * @param socketLane Ordinary socket lane.
 * @return True when the catalog owns this item socket lane.
 */
[[nodiscard]] bool owns_lane(std::uint16_t itemDefinitionIndex, std::uint8_t socketLane) noexcept;

/**
 * Applies a released catalyst plug and Masterwork bit as one checked item change.
 * Placeholder and non-catalyst items stay unchanged.
 * @param itemDefinitionIndex Native item definition index.
 * @param flags Candidate accumulated item-state bits.
 * @param plugs Candidate ordinary socket plugs.
 * @return Completed, unchanged, or failed without a partial change.
 */
[[nodiscard]] ApplyResult apply_completed(std::uint16_t itemDefinitionIndex,
                                          std::uint32_t& flags,
                                          std::span<std::optional<std::uint16_t>> plugs) noexcept;

/**
 * Adds acquired-state gates, completion flags, and completion values for released catalysts.
 * Existing authored rows with the same slot are raised to the required value. The input stays
 * unchanged when either fixed override bank cannot hold the complete deduplicated result.
 * @param family Candidate Family-5 state.
 * @return True when completion is disabled or every released override fits atomically.
 */
[[nodiscard]] bool append_investment_overrides(state::Family5State& family) noexcept;

/**
 * Raises account objective values required by released legacy catalysts.
 * The input stays unchanged if any derived objective is outside the supplied bank.
 * @param values Candidate account objective bank.
 * @return True when completion is disabled or every objective applies atomically.
 */
[[nodiscard]] bool append_objective_completions(std::span<std::int32_t> values) noexcept;

/**
 * Copies the complete catalog under its shared lock.
 * @param output Caller-owned fixed catalog storage.
 * @param count Receives the number of copied rows.
 * @return False only when output cannot hold the complete catalog.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Published catalyst record count. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::items::catalysts
