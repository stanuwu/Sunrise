#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../../middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::client::hooks::network::investment::lore {

namespace tables = middleware::content::packages::tables;

/** Every gate replaced here is one literal, so a longer expression is a different build. */
inline constexpr std::size_t kMaximumInstructions = 1;

/** One unlock expression instruction. */
struct Instruction {
    std::uint32_t opcode{};
    std::uint32_t operand{};

    friend bool operator==(const Instruction&, const Instruction&) = default;
};
// The patch reads and writes whole expression rows through this struct.
static_assert(sizeof(Instruction) == tables::kUnlockInstructionStride);
static_assert(offsetof(Instruction, opcode) == tables::kUnlockInstructionOpcodeOffset);
static_assert(offsetof(Instruction, operand) == tables::kUnlockInstructionOperandOffset);

/** One presentation gate: which record row holds it, which field, and the row's authored hash. */
struct Target {
    std::uint16_t row;
    std::uint16_t field;
    std::uint32_t hash;
};

/** Every presentation gate this build patches; the array size must match the rows added. */
inline constexpr auto kTargets = [] {
    std::array<Target, 24> targets{};
    std::size_t n = 0;
    // Record hashes of the Wish lore rows, in row order from 825.
    constexpr std::array<std::uint32_t, 15> wishes{0xFA360CA1U,
                                                   0xFA360CA2U,
                                                   0xFA360CA3U,
                                                   0xFA360CA4U,
                                                   0xFA360CA5U,
                                                   0xFA360CA6U,
                                                   0xFA360CA7U,
                                                   0xFA360CA8U,
                                                   0xFA360CA9U,
                                                   0xFB360E13U,
                                                   0xFB360E12U,
                                                   0xFB360E11U,
                                                   0xFB360E10U,
                                                   0xFB360E17U,
                                                   0xFB360E16U};
    for (std::size_t i = 0; i < wishes.size(); ++i) {
        targets[n++] = {
            static_cast<std::uint16_t>(825 + i), tables::kRecordCategoryExpressionField, wishes[i]};
    }
    // Record hashes of the Confessions chapter rows, in row order from 1708.
    constexpr std::array<std::uint32_t, 9> chapters{0xB780F393U,
                                                    0xB780F390U,
                                                    0xB780F391U,
                                                    0xB780F396U,
                                                    0xB780F397U,
                                                    0xB780F394U,
                                                    0xB780F395U,
                                                    0xB780F39AU,
                                                    0xB780F39BU};
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        targets[n++] = {static_cast<std::uint16_t>(1708 + i),
                        tables::kRecordAlternateExpressionField,
                        chapters[i]};
    }
    return targets;
}();

/**
 * Picks the one instruction that makes a shipped gate read false.
 * The whole expression must match this build before any byte of it is replaced.
 * @param code Complete shipped expression.
 * @param output Receives the replacement for the first instruction.
 * @return False when the shipped expression is not the literal true this build expects.
 */
[[nodiscard]] inline bool replacement(std::span<const Instruction> code,
                                      Instruction& output) noexcept {
    if (code.size() != 1 || code[0] != Instruction{tables::kUnlockLiteralOpcode, 1}) {
        return false;
    }
    output = Instruction{tables::kUnlockLiteralOpcode, 0};
    return true;
}

} // namespace sunrise::client::hooks::network::investment::lore
