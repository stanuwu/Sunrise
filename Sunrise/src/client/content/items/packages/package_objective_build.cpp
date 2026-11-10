#include "package_objective_build.h"

#include <cstdint>
#include <new>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/field_reader.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"
#include "package_pursuit_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace objectives = state::build_data::objectives;

/** Counts reported in the objective log line once the table is published. */
struct ObjectiveReport {
    std::size_t unreadable{};
    std::size_t unreadableAdjustments{};
    std::uint16_t firstUnreadableAdjustment{};
};

/**
 * How one row read ended. The installed data decides `unsupported` for good, and a failed
 * allocation is retried on a later pass.
 */
enum class RowRead : std::uint8_t { read, unsupported, retry };

/** The dense rows being read and the one instruction bank all their programs share. */
struct ObjectiveTable {
    std::vector<objectives::Row> rows;
    std::vector<state::unlocks::Instruction> bank;
    /** Each program is bound here, then appended to the bank. */
    std::vector<state::unlocks::Instruction> program;
};

/** Reads the completion controls of one bounded objective row. */
[[nodiscard]] bool read_completion_controls(std::span<const std::byte> blob,
                                            std::size_t at,
                                            objectives::Row& row) noexcept {
    return tables::read_boolean(blob, at + tables::kObjectiveOptionalOffset, row.optional)
           && tables::read_boolean(blob, at + tables::kObjectiveCountsDownOffset, row.countsDown);
}

/** Treats a full instruction bank as final and failed storage as retryable. */
[[nodiscard]] RowRead bank_refusal(BankFailure failure) noexcept {
    return failure == BankFailure::full ? RowRead::unsupported : RowRead::retry;
}

/** Moves the bound program into the shared bank. Overflowing the bank is final. */
[[nodiscard]] RowRead append_program(ObjectiveTable& table,
                                     state::build_data::rewards::Range& range) noexcept {
    if (table.program.size() > objectives::kInstructionCapacity - table.bank.size()) {
        return RowRead::unsupported;
    }
    try {
        range = {static_cast<std::uint32_t>(table.bank.size()),
                 static_cast<std::uint32_t>(table.program.size())};
        table.bank.insert(table.bank.end(), table.program.begin(), table.program.end());
    } catch (const std::bad_alloc&) {
        return RowRead::retry;
    }
    table.program.clear();
    return RowRead::read;
}

/**
 * Reads one objective row and binds its progress and target-adjustment programs.
 * An unbindable program leaves the row unreadable rather than refusing the table.
 */
[[nodiscard]] RowRead read_objective(const RewardConditions& conditions,
                                     std::span<const std::byte> blob,
                                     std::size_t at,
                                     ObjectiveTable& table,
                                     objectives::Row& row,
                                     ObjectiveReport& report) noexcept {
    if (!tables::read(blob, at, row.definitionHash)
        || !tables::read(blob, at + tables::kObjectiveCompletionValueOffset, row.completionValue)
        || !read_completion_controls(blob, at, row)) {
        return RowRead::unsupported;
    }
    state::build_data::rewards::Range bound{};
    BankFailure failure{};
    table.program.clear();
    row.adjustmentReadable = conditions.read(
        blob, at + tables::kObjectiveTargetAdjustmentField, table.program, bound, failure);
    if (failure != BankFailure::none) {
        return bank_refusal(failure);
    }
    if (!row.adjustmentReadable && report.unreadableAdjustments++ == 0) {
        report.firstUnreadableAdjustment = row.definitionIndex;
    }
    if (const RowRead appended = append_program(table, row.targetAdjustment);
        appended != RowRead::read) {
        return appended;
    }
    row.readable =
        conditions.read(
            blob, at + tables::kObjectiveSourceExpressionField, table.program, bound, failure)
        && !table.program.empty();
    if (failure != BankFailure::none) {
        return bank_refusal(failure);
    }
    if (!row.readable) {
        table.program.clear();
        ++report.unreadable;
        return RowRead::read;
    }
    row.itemValueSlot = conditions.item_value_slot(
        blob, at + tables::kObjectiveSourceExpressionField, row.itemValueMode);
    return append_program(table, row.expression);
}

} // namespace

/** Reads and publishes the objective table; one the data refuses settles the pursuit domains. */
bool build_objectives(const reader::Source& source,
                      reader::Scratch& scratch,
                      std::span<const std::byte> root,
                      const RewardConditions& conditions) noexcept {
    if (state::build_data::objective_definitions_ready()) {
        return true;
    }
    std::uint32_t tag = 0;
    if (!tables::slot_tag(root, tables::kObjectiveTableSlot, tag)
        || tables::package_of(tag) == tables::kAbsentPackageId) {
        settle_pursuits_unsupported("objective_slot");
        return false;
    }
    try {
        std::vector<std::byte> blob;
        tables::Array rows{};
        if (!reader::read_tag(source, scratch, tag, blob)) {
            return false;
        }
        if (!tables::find_array_at(blob, tables::kObjectiveRowArrayDescriptor, rows)
            || rows.elementClass != tables::kObjectiveRowClass || rows.count == 0
            || rows.count > objectives::kDefinitionCapacity || rows.dataOffset > blob.size()
            || rows.count > (blob.size() - rows.dataOffset) / tables::kObjectiveRowStride) {
            settle_pursuits_unsupported("objective_schema");
            return false;
        }
        // The programs bind through the condition tables. Once the reward domain has settled
        // those tables as unsupported, the pursuit domains settle too; a read that failed this
        // pass is retried on the next.
        if (!conditions.loaded()) {
            if (reward_definitions_settled() && !state::build_data::reward_definitions_ready()) {
                settle_pursuits_unsupported("condition_tables");
            }
            return false;
        }
        ObjectiveTable table;
        table.rows.resize(static_cast<std::size_t>(rows.count));
        ObjectiveReport report{};
        for (std::size_t i = 0; i < table.rows.size(); ++i) {
            table.rows[i].definitionIndex = static_cast<std::uint16_t>(i);
            const RowRead row = read_objective(conditions,
                                               blob,
                                               rows.dataOffset + i * tables::kObjectiveRowStride,
                                               table,
                                               table.rows[i],
                                               report);
            if (row == RowRead::retry) {
                return false;
            }
            if (row == RowRead::unsupported) {
                core::log::writef(core::log::Channel::client,
                                  core::log::Level::warn,
                                  "ev=pkg stage=objectives result=fail row=%zu",
                                  i);
                settle_pursuits_unsupported("objective_row");
                return false;
            }
        }
        const objectives::View view{table.rows, table.bank};
        if (!state::build_data::valid_objective_definitions(view)) {
            settle_pursuits_unsupported("objective_table");
            return false;
        }
        // Publishing can still fail on the cache write, which a later pass retries.
        if (!state::build_data::publish_objective_definitions(view)) {
            return false;
        }
        const bool complete = report.unreadable == 0 && report.unreadableAdjustments == 0;
        core::log::writef(core::log::Channel::client,
                          complete ? core::log::Level::info : core::log::Level::warn,
                          "ev=pkg stage=objectives result=ready rows=%zu unreadable=%zu "
                          "unreadable_adjustments=%zu first_unreadable_adjustment=%u",
                          table.rows.size(),
                          report.unreadable,
                          report.unreadableAdjustments,
                          static_cast<unsigned>(report.firstUnreadableAdjustment));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::client::content::items::packages
