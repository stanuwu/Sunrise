#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::inventory::buckets {

/** Clears every generated inventory-bucket routing descriptor. */
void clear() noexcept;

/**
 * Checks one complete inventory-bucket descriptor table.
 * @param descriptors Candidate installed-build routing descriptors.
 * @return True when every id, selector, and slot range is valid, and every id is unique.
 */
[[nodiscard]] bool valid(std::span<const Descriptor> descriptors) noexcept;

/**
 * Replaces the generated inventory-bucket descriptor table in one step.
 * @param descriptors Complete installed-build routing descriptors.
 * @return True when all descriptors pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Descriptor> descriptors) noexcept;

/**
 * Finds one inventory-bucket routing descriptor by native bucket id.
 * @param bucketId Native inventory-bucket id that is available.
 * @param descriptor Receives the matching routing descriptor.
 * @return True when the table holds that bucket.
 */
[[nodiscard]] bool find(std::uint8_t bucketId, Descriptor& descriptor) noexcept;

/**
 * Copies every descriptor in publication order.
 * @param output Caller-owned fixed descriptor storage.
 * @param count Receives the copied descriptor count, or zero when output is too small.
 * @return True when output can hold the whole table.
 */
[[nodiscard]] bool snapshot(std::span<Descriptor> output, std::size_t& count) noexcept;

/** @return Number of inventory-bucket descriptors, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::inventory::buckets
