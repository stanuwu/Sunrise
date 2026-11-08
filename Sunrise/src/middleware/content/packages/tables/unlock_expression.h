#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "../reader/typed_array.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * Reads the first operand of a given opcode out of one expression field.
 * The same field holds a value read on one row and a flag test on another, so the opcode is asked
 * for rather than assumed from the field.
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
    if (rowAt + field + kUnlockExpressionFieldSize > table.size()) {
        return false;
    }
    const std::size_t pointerAt = rowAt + field + kUnlockExpressionPointerOffset;
    std::memcpy(&count, table.data() + rowAt + field + kUnlockExpressionCountOffset, sizeof count);
    std::memcpy(&relative, table.data() + pointerAt, sizeof relative);
    if (count < 1 || count > kNodeExpressionCapacity) {
        return false;
    }
    const std::int64_t header = static_cast<std::int64_t>(pointerAt) + relative;
    reader::TypedArrayRows rows{};
    if (header < 0
        || !reader::read_typed_array(table,
                                     static_cast<std::size_t>(header),
                                     static_cast<std::uint64_t>(count),
                                     kInvestmentExpressionRowClass,
                                     kUnlockInstructionStride,
                                     rows)) {
        return false;
    }
    for (std::int64_t index = 0; index < count; ++index) {
        std::uint32_t instruction = 0;
        std::uint32_t operand = 0;
        const std::size_t at =
            rows.data + static_cast<std::size_t>(index) * kUnlockInstructionStride;
        std::memcpy(
            &instruction, table.data() + at + kUnlockInstructionOpcodeOffset, sizeof instruction);
        std::memcpy(&operand, table.data() + at + kUnlockInstructionOperandOffset, sizeof operand);
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
