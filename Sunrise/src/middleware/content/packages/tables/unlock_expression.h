#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "../reader/typed_array.h"
#include "definition_index_table.h"
#include "field_reader.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * Locates the instructions of one expression field.
 * @param table Blob the row sits in.
 * @param rowAt Byte offset of the row.
 * @param field Byte offset of the expression field within the row.
 * @param count Receives the instruction count; zero for an empty field.
 * @param rows Receives where the instructions start when there are any.
 * @return True when the field parses as an expression.
 */
[[nodiscard]] inline bool expression_rows(std::span<const std::byte> table,
                                          std::size_t rowAt,
                                          std::size_t field,
                                          std::int64_t& count,
                                          reader::TypedArrayRows& rows) noexcept {
    std::int64_t relative = 0;
    if (rowAt + field + kUnlockExpressionFieldSize > table.size()) {
        return false;
    }
    const std::size_t pointerAt = rowAt + field + kUnlockExpressionPointerOffset;
    std::memcpy(&count, table.data() + rowAt + field + kUnlockExpressionCountOffset, sizeof count);
    std::memcpy(&relative, table.data() + pointerAt, sizeof relative);
    if (count == 0) {
        return true;
    }
    if (count < 0 || count > kNodeExpressionCapacity) {
        return false;
    }
    const std::int64_t header = static_cast<std::int64_t>(pointerAt) + relative;
    return header >= 0
           && reader::read_typed_array(table,
                                       static_cast<std::size_t>(header),
                                       static_cast<std::uint64_t>(count),
                                       kInvestmentExpressionRowClass,
                                       kUnlockInstructionStride,
                                       rows);
}

/**
 * Reads one instruction of an expression located by expression_rows.
 * @return False for an opcode past the evaluator's, which means the field is not an expression.
 */
[[nodiscard]] inline bool expression_instruction(std::span<const std::byte> table,
                                                 const reader::TypedArrayRows& rows,
                                                 std::int64_t index,
                                                 std::uint32_t& opcode,
                                                 std::uint32_t& operand) noexcept {
    const std::size_t at = rows.data + static_cast<std::size_t>(index) * kUnlockInstructionStride;
    std::memcpy(&opcode, table.data() + at + kUnlockInstructionOpcodeOffset, sizeof opcode);
    std::memcpy(&operand, table.data() + at + kUnlockInstructionOperandOffset, sizeof operand);
    return opcode <= kUnlockOpcodeCeiling;
}

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
    reader::TypedArrayRows rows{};
    if (!expression_rows(table, rowAt, field, count, rows) || count < 1) {
        return false;
    }
    for (std::int64_t index = 0; index < count; ++index) {
        std::uint32_t instruction = 0;
        std::uint32_t operand = 0;
        if (!expression_instruction(table, rows, index, instruction, operand)) {
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

/**
 * Reads the operand of an expression that is one instruction of the given opcode and nothing else.
 * @param rows The expression's instruction array, as its field's descriptor located it.
 * @param operand Receives the operand; unchanged on failure.
 * @return False when the expression is anything else, an empty one included.
 */
[[nodiscard]] inline bool expression_lone_operand(std::span<const std::byte> blob,
                                                  const Array& rows,
                                                  std::uint32_t opcode,
                                                  std::uint32_t& operand) noexcept {
    std::uint32_t actual = 0;
    return rows.count == 1 && rows.elementClass == kInvestmentExpressionRowClass
           && read(blob, rows.dataOffset + kUnlockInstructionOpcodeOffset, actual)
           && actual == opcode
           && read(blob, rows.dataOffset + kUnlockInstructionOperandOffset, operand);
}

/**
 * Reads the value slot of an expression that is one value read and nothing else, the shape of a
 * plug's quantity expression.
 * @return False when the field is anything else.
 */
[[nodiscard]] inline bool expression_lone_value_slot(std::span<const std::byte> table,
                                                     std::size_t rowAt,
                                                     std::size_t field,
                                                     std::int16_t& slot) noexcept {
    Array rows{};
    std::uint32_t operand = 0;
    if (!find_array_at(table, rowAt + field, rows)
        || !expression_lone_operand(table, rows, kUnlockReadValueOpcode, operand)
        || operand > static_cast<std::uint32_t>(INT16_MAX)) {
        return false;
    }
    slot = static_cast<std::int16_t>(operand);
    return true;
}

/** @return How many operands a native opcode pops: none for reads and literals, else one or two. */
[[nodiscard]] constexpr std::size_t expression_operand_count(std::uint32_t opcode) noexcept {
    switch (opcode) {
    case kUnlockReadFlagOpcode:
    case kUnlockReadValueOpcode:
    case kUnlockLiteralOpcode:
    case kUnlockExpressionOpcode:
        return 0;
    case kUnlockNotOpcode:
    case kUnlockNegateOpcode:
    case kUnlockHashOpcode:
    case kUnlockBitNotOpcode:
        return 1;
    default:
        return 2;
    }
}

/**
 * Reads the one flag slot an expression requires set: a flag test that is the whole expression or
 * an operand of its outermost ANDs, outside any NOT.
 * @param slot Receives the slot, or -1 when the expression requires no flag or more than one.
 * @return False when the field does not parse as an expression.
 */
[[nodiscard]] inline bool expression_required_flag_slot(std::span<const std::byte> table,
                                                        std::size_t rowAt,
                                                        std::size_t field,
                                                        std::int16_t& slot) noexcept {
    // Each stack entry is the one flag its subexpression requires, none, or several.
    constexpr std::int32_t kNone = -1;
    constexpr std::int32_t kSeveral = -2;
    std::int64_t count = 0;
    reader::TypedArrayRows rows{};
    if (!expression_rows(table, rowAt, field, count, rows)) {
        return false;
    }
    std::array<std::int32_t, kNodeExpressionCapacity> stack{};
    std::size_t depth = 0;
    for (std::int64_t index = 0; index < count; ++index) {
        std::uint32_t opcode = 0;
        std::uint32_t operand = 0;
        if (!expression_instruction(table, rows, index, opcode, operand) || opcode == 0) {
            return false;
        }
        const std::size_t operands = expression_operand_count(opcode);
        if (depth < operands) {
            return false;
        }
        std::int32_t required = kNone;
        if (opcode == kUnlockReadFlagOpcode) {
            if (operand > static_cast<std::uint32_t>(INT16_MAX)) {
                return false;
            }
            required = static_cast<std::int32_t>(operand);
        } else if (opcode == kUnlockAndOpcode) {
            const std::int32_t left = stack[depth - 2];
            const std::int32_t right = stack[depth - 1];
            required = left == kNone ? right : right == kNone ? left : kSeveral;
        }
        depth -= operands;
        stack[depth++] = required;
    }
    if (count != 0 && depth != 1) {
        return false;
    }
    slot = count != 0 && stack[0] >= 0 ? static_cast<std::int16_t>(stack[0]) : std::int16_t{-1};
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
