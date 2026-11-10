#pragma once

#include "definition.h"

namespace sunrise::state::build_data::objectives {

/** Discards the objective table and its instruction bank. */
void clear() noexcept;
/** True once the objective table has been published. */
[[nodiscard]] bool ready() noexcept;
/** Checks row order, program ranges and every instruction. */
[[nodiscard]] bool valid(View data) noexcept;
/** Publishes the validated table and bank together under the catalog lock. */
[[nodiscard]] bool replace(View data) noexcept;
/** Copies one row with its programs, returning false when the row is missing or the copy fails. */
[[nodiscard]] bool find(std::uint16_t index, Definition& output) noexcept;
/** Copies one row's fixed fields and leaves its programs in the bank. */
[[nodiscard]] bool find_row(std::uint16_t index, Row& output) noexcept;
/** @return Published row count, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

/** Copies the complete table or bank; count is zero if the output is too small. */
[[nodiscard]] bool snapshot(std::span<Row> output, std::size_t& count) noexcept;
[[nodiscard]] bool snapshot(std::span<unlocks::Instruction> output, std::size_t& count) noexcept;

} // namespace sunrise::state::build_data::objectives
