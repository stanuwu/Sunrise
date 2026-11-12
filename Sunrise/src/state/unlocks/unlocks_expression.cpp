#include "unlocks_expression.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../account/account_state.h"
#include "definition.h"

namespace sunrise::state::unlocks {
namespace {

/** Fixed evaluation stack, which refuses rather than growing. */
class Stack final {
public:
    /** @param value Value to push. @return True when there was room. */
    [[nodiscard]] bool push(std::int32_t value) noexcept {
        if (depth_ == values_.size()) {
            return false;
        }
        values_[depth_++] = value;
        return true;
    }

    /** @param value Receives the top value. @return True when there was one. */
    [[nodiscard]] bool pop(std::int32_t& value) noexcept {
        if (depth_ == 0) {
            return false;
        }
        value = values_[--depth_];
        return true;
    }

    /** @return Values currently held. */
    [[nodiscard]] std::size_t depth() const noexcept {
        return depth_;
    }

private:
    std::array<std::int32_t, kExpressionStackCapacity> values_{};
    std::size_t depth_{};
};

/** FNV-1a starts from the standard 32-bit offset basis. */
constexpr std::uint32_t kFnvOffsetBasis = 0x811C9DC5U;
/** FNV-1a multiplies by the standard 32-bit prime after each byte. */
constexpr std::uint32_t kFnvPrime = 0x01000193U;

/** @return The bits of a value, so arithmetic wraps rather than overflowing. */
[[nodiscard]] constexpr std::uint32_t bits(std::int32_t value) noexcept {
    return static_cast<std::uint32_t>(value);
}

/** @return FNV-1a of the four bytes of value, most significant first, from seed. */
[[nodiscard]] std::int32_t fnv1a_word(std::uint32_t seed, std::uint32_t value) noexcept {
    std::uint32_t hash = seed;
    for (std::size_t byte = sizeof value; byte-- > 0;) {
        hash = (hash ^ ((value >> (byte * CHAR_BIT)) & UINT8_MAX)) * kFnvPrime;
    }
    return static_cast<std::int32_t>(hash);
}

/**
 * Applies a quotient or a remainder the way the client's signed divide does.
 * @return False for the one quotient that overflows; a zero divisor yields 0.
 */
[[nodiscard]] bool division_result(Opcode opcode,
                                   std::int32_t left,
                                   std::int32_t right,
                                   std::int32_t& value) noexcept {
    if (right == 0) {
        value = 0;
        return true;
    }
    // The client's divide traps on this pair, so no installed condition can depend on it.
    if (left == (std::numeric_limits<std::int32_t>::min)() && right == -1) {
        return false;
    }
    value = opcode == Opcode::divide ? left / right : left % right;
    return true;
}

/**
 * Applies one binary operator.
 * @return False when the opcode is not a binary operator or its result overflows.
 */
[[nodiscard]] bool
binary_result(Opcode opcode, std::int32_t left, std::int32_t right, std::int32_t& value) noexcept {
    switch (opcode) {
    case Opcode::logicalOr:
        value = left != 0 || right != 0;
        return true;
    case Opcode::logicalAnd:
        value = left != 0 && right != 0;
        return true;
    case Opcode::logicalNor:
        value = left == 0 && right == 0;
        return true;
    case Opcode::logicalNand:
        value = left == 0 || right == 0;
        return true;
    case Opcode::equal:
        value = left == right;
        return true;
    case Opcode::notEqual:
    case Opcode::notEqualAlias:
        value = left != right;
        return true;
    case Opcode::greaterThan:
        value = left > right;
        return true;
    case Opcode::greaterOrEqual:
        value = left >= right;
        return true;
    case Opcode::lessThan:
        value = left < right;
        return true;
    case Opcode::lessOrEqual:
        value = left <= right;
        return true;
    case Opcode::add:
        value = static_cast<std::int32_t>(bits(left) + bits(right));
        return true;
    case Opcode::subtract:
        value = static_cast<std::int32_t>(bits(left) - bits(right));
        return true;
    case Opcode::multiply:
        value = static_cast<std::int32_t>(bits(left) * bits(right));
        return true;
    case Opcode::divide:
    case Opcode::remainder:
        return division_result(opcode, left, right, value);
    case Opcode::hashCombine:
        value = fnv1a_word(bits(left), bits(right));
        return true;
    case Opcode::bitAnd:
        value = static_cast<std::int32_t>(bits(left) & bits(right));
        return true;
    case Opcode::bitOr:
        value = static_cast<std::int32_t>(bits(left) | bits(right));
        return true;
    case Opcode::bitXor:
        value = static_cast<std::int32_t>(bits(left) ^ bits(right));
        return true;
    default:
        return false;
    }
}

/**
 * Runs one instruction against the stack.
 * @param instruction Instruction to run.
 * @param inputs Flag and value readers.
 * @param stack Evaluation stack.
 * @return True when the opcode is known and its operands were there.
 */
[[nodiscard]] bool
step(const Instruction& instruction, const Inputs& inputs, Stack& stack) noexcept {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t value = 0;
    switch (instruction.opcode) {
    case Opcode::flag: {
        bool set = false;
        return inputs.flag(inputs.context, instruction, set) && stack.push(set ? 1 : 0);
    }
    case Opcode::loadValue:
        return inputs.value(inputs.context, instruction, value) && stack.push(value);
    case Opcode::constant:
        return stack.push(static_cast<std::int32_t>(instruction.operand));
    case Opcode::logicalNot:
        return stack.pop(left) && stack.push(left == 0 ? 1 : 0);
    case Opcode::negate:
        return stack.pop(left) && stack.push(static_cast<std::int32_t>(0U - bits(left)));
    case Opcode::hash:
        return stack.pop(left) && stack.push(fnv1a_word(kFnvOffsetBasis, bits(left)));
    case Opcode::bitNot:
        return stack.pop(left) && stack.push(static_cast<std::int32_t>(~bits(left)));
    default:
        // The right operand was pushed last, so it comes off first.
        return stack.pop(right) && stack.pop(left)
               && binary_result(instruction.opcode, left, right, value) && stack.push(value);
    }
}

} // namespace

/**
 * Converts one native opcode word to an evaluated opcode.
 * @param opcode Receives the opcode; unchanged on failure.
 * @return False for the shared-expression opcode and any word the client does not evaluate.
 */
bool decode_opcode(std::uint32_t native, Opcode& opcode) noexcept {
    if (native > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    switch (static_cast<Opcode>(native)) {
    case Opcode::flag:
    case Opcode::logicalNot:
    case Opcode::logicalOr:
    case Opcode::logicalAnd:
    case Opcode::logicalNor:
    case Opcode::notEqual:
    case Opcode::logicalNand:
    case Opcode::equal:
    case Opcode::notEqualAlias:
    case Opcode::loadValue:
    case Opcode::constant:
    case Opcode::greaterThan:
    case Opcode::greaterOrEqual:
    case Opcode::lessThan:
    case Opcode::lessOrEqual:
    case Opcode::add:
    case Opcode::subtract:
    case Opcode::multiply:
    case Opcode::divide:
    case Opcode::remainder:
    case Opcode::negate:
    case Opcode::hash:
    case Opcode::hashCombine:
    case Opcode::bitAnd:
    case Opcode::bitOr:
    case Opcode::bitXor:
    case Opcode::bitNot:
        opcode = static_cast<Opcode>(native);
        return true;
    default:
        return false;
    }
}

/**
 * Checks one instruction's bank and its operand against that bank's capacity.
 * @return False for an unknown opcode, a bank the opcode cannot read, or an operand past it.
 */
bool valid(const Instruction& instruction) noexcept {
    Opcode decoded{};
    if (!decode_opcode(static_cast<std::uint32_t>(instruction.opcode), decoded)) {
        return false;
    }
    const std::uint32_t index = instruction.operand;
    if (instruction.opcode == Opcode::flag) {
        switch (instruction.bank) {
        case Bank::account:
            return index < kAccountFlagCapacity;
        case Bank::profile:
            return index < kProfileFlagCapacity;
        case Bank::character:
            return index < kCharacterObjectFlagCapacity;
        case Bank::characterClass:
            return index < kCharacterClassCount;
        case Bank::external:
            return true;
        default:
            return false;
        }
    }
    if (instruction.opcode == Opcode::loadValue) {
        switch (instruction.bank) {
        case Bank::account:
            return index < kObjectiveValueCapacity;
        case Bank::character:
            return index < kCharacterObjectValueCapacity;
        case Bank::external:
            return true;
        default:
            return false;
        }
    }
    return instruction.bank == Bank::none;
}

/**
 * Evaluates one expression program to its final value.
 * @param result Receives the final value; cleared on any refusal.
 * @return False for an empty program, a refused step, or a final stack other than one value.
 */
bool evaluate(std::span<const Instruction> program,
              const Inputs& inputs,
              std::int32_t& result) noexcept {
    result = 0;
    if (program.empty() || inputs.flag == nullptr || inputs.value == nullptr) {
        return false;
    }
    Stack stack;
    for (const Instruction& instruction : program) {
        if (!step(instruction, inputs, stack)) {
            return false;
        }
    }
    std::int32_t final = 0;
    if (stack.depth() != 1 || !stack.pop(final)) {
        return false;
    }
    result = final;
    return true;
}

/**
 * Evaluates one condition program, which holds when its final value is nonzero.
 * @param result Receives whether the final value is nonzero; cleared on any refusal.
 */
bool evaluate(std::span<const Instruction> program, const Inputs& inputs, bool& result) noexcept {
    std::int32_t value = 0;
    const bool evaluated = evaluate(program, inputs, value);
    result = evaluated && value != 0;
    return evaluated;
}

} // namespace sunrise::state::unlocks
