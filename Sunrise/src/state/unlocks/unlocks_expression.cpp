#include "unlocks_expression.h"

#include <array>
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

/** Signed arithmetic wraps rather than overflowing. */
[[nodiscard]] std::int32_t wrapping_add(std::int32_t left, std::int32_t right) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(left)
                                     + static_cast<std::uint32_t>(right));
}

/**
 * Applies one binary operator.
 * @return False when the opcode is not a binary operator.
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
    case Opcode::equal:
        value = left == right;
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
        value = wrapping_add(left, right);
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
        return stack.pop(left) && stack.push(wrapping_add(~left, 1));
    default:
        // The right operand was pushed last, so it comes off first.
        return stack.pop(right) && stack.pop(left)
               && binary_result(instruction.opcode, left, right, value) && stack.push(value);
    }
}

} // namespace

/** Converts one native opcode word to an evaluated opcode. */
bool decode_opcode(std::uint32_t native, Opcode& opcode) noexcept {
    if (native > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    switch (static_cast<Opcode>(native)) {
    case Opcode::flag:
    case Opcode::logicalNot:
    case Opcode::logicalOr:
    case Opcode::logicalAnd:
    case Opcode::equal:
    case Opcode::loadValue:
    case Opcode::constant:
    case Opcode::greaterThan:
    case Opcode::greaterOrEqual:
    case Opcode::lessThan:
    case Opcode::lessOrEqual:
    case Opcode::add:
    case Opcode::negate:
        opcode = static_cast<Opcode>(native);
        return true;
    default:
        return false;
    }
}

/** Checks one instruction's bank and its operand against that bank's capacity. */
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

/** Evaluates one expression program. */
bool evaluate(std::span<const Instruction> program, const Inputs& inputs, bool& result) noexcept {
    result = false;
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
    result = final != 0;
    return true;
}

} // namespace sunrise::state::unlocks
