#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * Reading unlock expressions out of definition rows.
 *
 * A row field holds a count and, eight bytes on, a self-relative offset to a run of instructions.
 * Each instruction is an opcode then an operand. Which field a row uses varies, and so does what it
 * carries: the same field holds a value read on one row and a flag test on another, so both readers
 * are tried against the same fields rather than each field being treated as fixed-purpose.
 */

/**
 * Reads the first operand of a given opcode out of one expression field.
 * @param table Blob the row sits in.
 * @param rowAt Byte offset of the row.
 * @param field Byte offset of the expression field within the row.
 * @param opcode Opcode to look for: kUnlockReadValueOpcode or kUnlockReadFlagOpcode.
 * @param slot Receives the operand when one is found.
 * @return True when the field parses as an expression and names that opcode.
 */
[[nodiscard]] inline bool expression_operand(std::span<const std::byte> table,
                                             std::size_t rowAt,
                                             std::size_t field,
                                             std::uint32_t opcode,
                                             std::int16_t& slot) noexcept {
    std::int64_t count = 0;
    std::int64_t relative = 0;
    if (rowAt + field + 16 > table.size()) {
        return false;
    }
    std::memcpy(&count, table.data() + rowAt + field, sizeof count);
    std::memcpy(&relative, table.data() + rowAt + field + 8, sizeof relative);
    if (count < 1 || count > kNodeExpressionCapacity) {
        return false;
    }
    const std::size_t pointerAt = rowAt + field + 8;
    const std::int64_t target = static_cast<std::int64_t>(pointerAt) + relative
                                + static_cast<std::int64_t>(kHeaderSkip);
    if (target < 0
        || static_cast<std::size_t>(target) + static_cast<std::size_t>(count) * kUnlockInstructionStride
               > table.size()) {
        return false;
    }
    const auto base = static_cast<std::size_t>(target);
    for (std::int64_t index = 0; index < count; ++index) {
        std::uint32_t instruction = 0;
        std::uint32_t operand = 0;
        const std::size_t at = base + static_cast<std::size_t>(index) * kUnlockInstructionStride;
        std::memcpy(&instruction, table.data() + at, sizeof instruction);
        std::memcpy(&operand, table.data() + at + 4, sizeof operand);
        if (instruction > kUnlockOpcodeCeiling) {
            return false;
        }
        if (instruction == opcode && operand <= static_cast<std::uint32_t>(INT16_MAX)) {
            slot = static_cast<std::int16_t>(operand);
            return true;
        }
    }
    return false;
}

/** Reads the value slot one expression field names, or reports that it names none. */
[[nodiscard]] inline bool expression_value_slot(std::span<const std::byte> table,
                                                std::size_t rowAt,
                                                std::size_t field,
                                                std::int16_t& slot) noexcept {
    return expression_operand(table, rowAt, field, kUnlockReadValueOpcode, slot);
}

/** Reads the flag slot one expression field tests, or reports that it tests none. */
[[nodiscard]] inline bool expression_flag_slot(std::span<const std::byte> table,
                                               std::size_t rowAt,
                                               std::size_t field,
                                               std::int16_t& slot) noexcept {
    return expression_operand(table, rowAt, field, kUnlockReadFlagOpcode, slot);
}

} // namespace sunrise::middleware::content::packages::tables
