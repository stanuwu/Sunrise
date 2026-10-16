#pragma once

#include <span>

#include "definition.h"

namespace sunrise::state::build_data::season_pass {

/** Discards the installed pass, including its native claim indices and wrapper packages. */
void clear() noexcept;

/**
 * Checks one complete pass catalog without publishing it.
 * @param rewards Reward rows in native claim order; unavailable rows retain their positions.
 * @param packages Wrapper packages referenced by reward items.
 * @return True when both banks fit fixed storage and every populated row is valid.
 */
[[nodiscard]] bool valid(std::span<const Reward> rewards,
                         std::span<const Package> packages) noexcept;

/**
 * Replaces the complete pass catalog without reordering native claim indices.
 * @param rewards Complete reward rows in native claim order.
 * @param packages Complete wrapper packages.
 * @return True when both banks validate and publish together.
 */
[[nodiscard]] bool replace(std::span<const Reward> rewards,
                           std::span<const Package> packages) noexcept;

/**
 * Copies one available native claim row.
 * @param rewardIndex Native claim-row index.
 * @param reward Receives the row, or is cleared when absent or unavailable.
 * @return True when the index names an available row.
 */
[[nodiscard]] bool find(std::uint16_t rewardIndex, Reward& reward) noexcept;

/**
 * Finds the wrapper package one item hash opens into.
 * @param definitionHash Authored wrapper item hash.
 * @param package Receives the package, or is cleared when absent.
 * @return True when the catalog contains that wrapper.
 */
[[nodiscard]] bool find_package(std::uint32_t definitionHash, Package& package) noexcept;

/**
 * Copies the complete pass in native claim order.
 * @param output Caller-owned reward storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every reward row.
 */
[[nodiscard]] bool snapshot(std::span<Reward> output, std::size_t& count) noexcept;

/**
 * Copies every wrapper package in publication order.
 * @param output Caller-owned package storage.
 * @param count Receives the copied package count, or zero when output is too small.
 * @return True when output can hold every package.
 */
[[nodiscard]] bool snapshot_packages(std::span<Package> output, std::size_t& count) noexcept;

/** @return Number of native claim positions, including unavailable rows. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::season_pass
