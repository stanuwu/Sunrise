#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::unlocks {

/** Stack slots one expression may use. The deepest installed reward condition needs nine. */
inline constexpr std::size_t kExpressionStackCapacity = 16;

/** Opcodes the client's expression evaluator implements that the server evaluates. */
enum class Opcode : std::uint8_t {
    /** Pushes 1 when the bound flag is set, otherwise 0. */
    flag = 1,
    /** Pops one value and pushes 1 when it was zero. */
    logicalNot = 2,
    /** Binary operators pop right then left and push the result. */
    logicalOr = 3,
    logicalAnd = 4,
    equal = 8,
    /** Pushes one bound value. */
    loadValue = 10,
    /** Pushes the operand as a signed constant. */
    constant = 11,
    greaterThan = 13,
    greaterOrEqual = 14,
    lessThan = 15,
    lessOrEqual = 16,
    add = 17,
    /** Pops one value and pushes its two's-complement negation. */
    negate = 22,
};

/** Saved bank a flag or value instruction reads once its authored unlock slot is bound. */
enum class Bank : std::uint8_t {
    /** Operators and constants read no bank. */
    none,
    account,
    profile,
    character,
    /** A flag that is set when the operand names the selected character's class. */
    characterClass,
    /** Resolved by the caller; the operand is its reward identity or native vendor slot. */
    external,
};

/** One bound expression instruction. */
struct Instruction {
    Opcode opcode{};
    Bank bank{Bank::none};
    /** Bank index, class index, caller-owned external operand, or signed constant. */
    std::uint32_t operand{};
};

/** Reads one bound flag. Returns false when the flag cannot be read. */
using FlagReader = bool (*)(const void* context,
                            const Instruction& instruction,
                            bool& set) noexcept;
/** Reads one bound value. Returns false when the value cannot be read. */
using ValueReader = bool (*)(const void* context,
                             const Instruction& instruction,
                             std::int32_t& value) noexcept;

/** The two readers one evaluation needs, and the caller state they share. */
struct Inputs {
    FlagReader flag{};
    ValueReader value{};
    const void* context{};
};

/**
 * Converts one native opcode word to an evaluated opcode.
 * @return False for an opcode the server does not evaluate.
 */
[[nodiscard]] bool decode_opcode(std::uint32_t native, Opcode& opcode) noexcept;

/** Checks one instruction's bank and its operand against that bank's capacity. */
[[nodiscard]] bool valid(const Instruction& instruction) noexcept;

/**
 * Evaluates one expression program.
 * Every failure is refused, never defaulted: bad opcode, bad stack, unreadable flag or value.
 * @param program Instructions in evaluation order.
 * @param inputs Flag and value readers.
 * @param result Receives whether the final value is nonzero, and is cleared on any refusal.
 * @return True only when the whole program evaluated to one value.
 */
[[nodiscard]] bool
evaluate(std::span<const Instruction> program, const Inputs& inputs, bool& result) noexcept;

} // namespace sunrise::state::unlocks
