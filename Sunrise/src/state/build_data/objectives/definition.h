#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../../unlocks/unlocks_expression.h"
#include "../rewards/definition.h"

namespace sunrise::state::build_data::objectives {

/** The shipped build declares 8,483 objectives. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 16384;
/**
 * The shipped build's programs expand to at most 117,168 instructions: 52,204 progress and
 * 64,964 target adjustment. The bank leaves room above that.
 */
inline constexpr std::size_t kInstructionCapacity = 131072;
/** All bits set marks an objective whose progress the held item does not carry. */
inline constexpr std::uint16_t kNoItemValueSlot = 0xFFFF;

/** Which held instances the client reads an item-held value slot from. */
enum class ItemValueMode : std::uint8_t {
    /** The held item in context. */
    context,
    /** The largest value the slot holds across the character's held instances that carry it. */
    maximum,
};

/** One stored objective row, whose two programs are ranges of the instruction bank. */
struct Row {
    std::uint32_t definitionHash{};
    std::int32_t completionValue{};
    std::uint16_t definitionIndex{};
    bool optional{};
    bool countsDown{};
    /** False when the optional target adjustment could not be bound. A row may have none. */
    bool adjustmentReadable{};
    /** The native evaluator subtracts this program's value from completionValue. */
    rewards::Range targetAdjustment{};
    /** False when the installed progress expression could not be bound. */
    bool readable{};
    rewards::Range expression{};
    /** Value slot the client reads from the held item instead of a bank, or kNoItemValueSlot. */
    std::uint16_t itemValueSlot{kNoItemValueSlot};
    /** Context for every objective without an item-held value slot. */
    ItemValueMode itemValueMode{};
};

/** The dense objective table and the instruction bank its rows name. */
struct View {
    std::span<const Row> rows;
    std::span<const unlocks::Instruction> instructions;
};

/** One objective row with its programs copied out of the bank. */
struct Definition {
    std::uint32_t definitionHash{};
    std::int32_t completionValue{};
    std::uint16_t definitionIndex{};
    bool optional{};
    bool countsDown{};
    /** False when the optional target adjustment could not be bound. A row may have none. */
    bool adjustmentReadable{};
    /** The native evaluator subtracts this expression's value from completionValue. */
    std::vector<unlocks::Instruction> targetAdjustment;
    /** False when the installed progress expression could not be bound. */
    bool readable{};
    std::vector<unlocks::Instruction> expression;
    /** Value slot the client reads from the held item instead of a bank, or kNoItemValueSlot. */
    std::uint16_t itemValueSlot{kNoItemValueSlot};
    /** Context for every objective without an item-held value slot. */
    ItemValueMode itemValueMode{};
};

} // namespace sunrise::state::build_data::objectives
