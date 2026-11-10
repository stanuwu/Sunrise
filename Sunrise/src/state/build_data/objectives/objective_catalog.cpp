#include "objective_catalog.h"

#include <algorithm>
#include <mutex>
#include <new>
#include <shared_mutex>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::objectives {
namespace {

core::threading::SrwLock g_lock;
Table<Row, kDefinitionCapacity> g_rows;
Table<unlocks::Instruction, kInstructionCapacity> g_instructions;

/**
 * A readable row has a progress program, an unreadable adjustment leaves its range empty, only a
 * readable program can name an item-held value, and only an item-held value reads past its context.
 */
[[nodiscard]] bool
valid_row(const Row& row, std::size_t index, std::span<const unlocks::Instruction> bank) noexcept {
    return row.definitionIndex == index && rewards::fits(row.expression, bank)
           && rewards::fits(row.targetAdjustment, bank)
           && row.readable == (row.expression.count != 0)
           && (row.adjustmentReadable || row.targetAdjustment.count == 0)
           && (row.readable || row.itemValueSlot == kNoItemValueSlot)
           && (row.itemValueSlot != kNoItemValueSlot
               || row.itemValueMode == ItemValueMode::context);
}

/** Copies one stored program out of the bank. The copy may throw on allocation. */
[[nodiscard]] std::vector<unlocks::Instruction> program(rewards::Range range) {
    const auto rows = g_instructions.rows().subspan(range.first, range.count);
    return {rows.begin(), rows.end()};
}

} // namespace

/** Discards the objective table and its instruction bank. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_rows.clear();
    g_instructions.clear();
}

/** @return True once the objective table has been published. */
bool ready() noexcept {
    const std::shared_lock guard(g_lock);
    return g_rows.count() != 0;
}

/** Checks row order, program ranges and every instruction. */
bool valid(View data) noexcept {
    if (data.rows.empty() || data.rows.size() > kDefinitionCapacity
        || data.instructions.size() > kInstructionCapacity
        || !std::all_of(data.instructions.begin(), data.instructions.end(), unlocks::valid)) {
        return false;
    }
    for (std::size_t index = 0; index < data.rows.size(); ++index) {
        if (!valid_row(data.rows[index], index, data.instructions)) {
            return false;
        }
    }
    return true;
}

/** Publishes the validated table and bank together under the catalog lock. */
bool replace(View data) noexcept {
    if (!valid(data)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    return g_rows.replace(data.rows) && g_instructions.replace(data.instructions);
}

/** Copies one row with its programs, returning false when the row is missing or the copy fails. */
bool find(std::uint16_t index, Definition& output) noexcept {
    output = {};
    const std::shared_lock guard(g_lock);
    if (index >= g_rows.count()) {
        return false;
    }
    const Row& row = g_rows.rows()[index];
    try {
        output.targetAdjustment = program(row.targetAdjustment);
        output.expression = program(row.expression);
    } catch (const std::bad_alloc&) {
        output = {};
        return false;
    }
    output.definitionHash = row.definitionHash;
    output.completionValue = row.completionValue;
    output.definitionIndex = row.definitionIndex;
    output.optional = row.optional;
    output.countsDown = row.countsDown;
    output.adjustmentReadable = row.adjustmentReadable;
    output.readable = row.readable;
    output.itemValueSlot = row.itemValueSlot;
    output.itemValueMode = row.itemValueMode;
    return true;
}

/** Copies one row's fixed fields and leaves its programs in the bank. */
bool find_row(std::uint16_t index, Row& output) noexcept {
    output = {};
    const std::shared_lock guard(g_lock);
    if (index >= g_rows.count()) {
        return false;
    }
    output = g_rows.rows()[index];
    return true;
}

/** @return Published row count, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_rows.count();
}

/** Copies every row; count is zero when the output is too small. */
bool snapshot(std::span<Row> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_rows.snapshot(output, count);
}

/** Copies every program instruction; count is zero when the output is too small. */
bool snapshot(std::span<unlocks::Instruction> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_instructions.snapshot(output, count);
}

} // namespace sunrise::state::build_data::objectives
