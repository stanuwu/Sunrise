#include "package_reward_build.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808075.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/field_reader.h"
#include "../../../../middleware/content/packages/tables/reward_table.h"
#include "../../../../middleware/content/packages/tables/unlock_expression.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/unlocks/unlocks_expression.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace layout = middleware::content::packages::tables::rewards;
namespace domain = state::build_data::rewards;
namespace unlocks = state::unlocks;

/** Logs one refused reward load, sizing or publication. */
void report_refusal(const char* reason) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=rewards result=fail reason=%s",
                      reason);
}

/** Logs a refusal the installed data decides and settles the domain in State once. */
void settle_unsupported(const char* reason) noexcept {
    report_refusal(reason);
    if (!state::build_data::reward_definitions_unsupported()) {
        state::build_data::settle_reward_definitions_unsupported();
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=pkg stage=rewards result=unsupported");
    }
}

/** An unread item row keeps a zero hash. */
[[nodiscard]] bool item_read(std::span<const domain::Item> items, std::uint16_t index) noexcept {
    return index < items.size() && items[index].definitionHash != 0;
}

/** Appends within a bank's capacity; an allocation failure is refused rather than thrown. */
template <typename T> bool append(std::vector<T>& bank, T value, std::size_t capacity) noexcept {
    if (bank.size() >= capacity) {
        return false;
    }
    try {
        bank.push_back(value);
        return true;
    } catch (...) {
        return false;
    }
}

/** Package reads can fail temporarily. Only decoded schema failures settle a domain. */
enum class TableRead { loaded, readFailed, unsupported };

/**
 * Reads the table an investment-root slot names, checking its class when one is expected.
 * @param expectedClass Required table class, or zero to accept any class.
 * @return Unsupported for an absent slot or a wrong class; readFailed for a package read error.
 */
TableRead root_table(const reader::Source& source,
                     reader::Scratch& scratch,
                     std::span<const std::byte> root,
                     std::size_t slot,
                     std::vector<std::byte>& blob,
                     std::uint32_t expectedClass) noexcept {
    std::uint32_t tag = 0;
    std::uint32_t cls = 0;
    if (!tables::slot_tag(root, slot, tag) || tables::package_of(tag) == tables::kAbsentPackageId) {
        return TableRead::unsupported;
    }
    if (!reader::read_tag(source, scratch, tag, blob, cls)) {
        return TableRead::readFailed;
    }
    return expectedClass == 0 || cls == expectedClass ? TableRead::loaded : TableRead::unsupported;
}

/** Required reward tables settle only after a decoded schema failure, leaving reads retryable. */
bool load_reward_table(const reader::Source& source,
                       reader::Scratch& scratch,
                       std::span<const std::byte> root,
                       std::size_t slot,
                       std::vector<std::byte>& blob,
                       std::uint32_t expectedClass,
                       const char* reason) noexcept {
    const auto result = root_table(source, scratch, root, slot, blob, expectedClass);
    if (result == TableRead::unsupported) {
        settle_unsupported(reason);
    } else if (result == TableRead::readFailed) {
        report_refusal(reason);
    }
    return result == TableRead::loaded;
}

/** Reads one entry's fixed fields; a negative or non-finite weight is refused. */
bool read_reward_entry(std::span<const std::byte> blob,
                       std::size_t at,
                       domain::Entry& entry) noexcept {
    return tables::read(blob, at + layout::EntryRow::kItem, entry.itemIndex)
           && tables::read(blob, at + layout::EntryRow::kQuantity, entry.quantity)
           && tables::read(blob, at + layout::EntryRow::kPool, entry.poolIndex)
           && tables::read(blob, at + layout::EntryRow::kSupplemental, entry.supplementalIndex)
           && tables::read(blob, at + layout::EntryRow::kCategory, entry.categoryHash)
           && tables::read(blob, at + layout::EntryRow::kWeight, entry.weight)
           && std::isfinite(entry.weight) && entry.weight >= 0;
}

/**
 * Reads the single flag that gates use of the class's default finisher.
 * @param blob Finisher item definition bytes.
 * @param output Receives the flag slot the gate reads.
 * @return False unless the gate is exactly one flag read.
 */
bool read_class_flag(std::span<const std::byte> blob, std::uint16_t& output) noexcept {
    std::size_t at = 0;
    std::uint32_t cls = 0;
    tables::Array expression{};
    if (!tables::relative(blob, layout::ItemFields::kUseCondition, at)
        || !reader::layout::class_word_before(blob, at, cls) || cls != layout::UseCondition::kClass
        || !tables::read_array(blob,
                               at,
                               tables::kInvestmentExpressionRowClass,
                               tables::kUnlockInstructionStride,
                               expression)
        || expression.count != 1) {
        return false;
    }
    std::uint32_t opcode = 0;
    std::uint32_t flag = domain::kAbsent;
    if (!tables::read(blob, expression.dataOffset + tables::kUnlockInstructionOpcodeOffset, opcode)
        || opcode != tables::kUnlockReadFlagOpcode
        || !tables::read(
            blob, expression.dataOffset + tables::kUnlockInstructionOperandOffset, flag)
        || flag >= domain::kAbsent) {
        return false;
    }
    output = static_cast<std::uint16_t>(flag);
    return true;
}

} // namespace

/** @return True when the reward graph is published or can never be. */
bool reward_definitions_settled() noexcept {
    return state::build_data::reward_definitions_ready()
           || state::build_data::reward_definitions_unsupported();
}

/**
 * Appends one pool entry with its condition, modifiers and socket overrides.
 * @param blob Pool table bytes.
 * @param at Offset of the entry row.
 * @param failure Receives which bank refused, or none for an unreadable entry.
 * @param reason Receives the refusing bank's label when a bank refused.
 * @return False when the entry is unreadable or a bank refused it.
 */
bool RewardBuild::entry(std::span<const std::byte> blob,
                        std::size_t at,
                        BankFailure& failure,
                        const char*& reason) noexcept {
    domain::Entry out{};
    failure = BankFailure::none;
    if (!read_reward_entry(blob, at, out)
        || !conditions.read(
            blob, at + layout::EntryRow::kCondition, instructions_, out.condition, failure)) {
        reason = failure == BankFailure::full ? "instruction_capacity" : "instruction_storage";
        return false;
    }
    out.supplementalMissing = supplementalMissing_ && out.supplementalIndex != domain::kAbsent;
    tables::Array rows{};
    if (!tables::read_array(blob,
                            at + layout::EntryRow::kModifiers,
                            layout::ModifierRow::kClass,
                            layout::ModifierRow::kSize,
                            rows)) {
        return false;
    }
    out.modifiers = {static_cast<std::uint32_t>(modifiers_.size()),
                     static_cast<std::uint32_t>(rows.count)};
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto offset = rows.dataOffset + i * layout::ModifierRow::kSize;
        domain::Modifier modifier{};
        if (!conditions.read(blob,
                             offset + layout::ModifierRow::kCondition,
                             instructions_,
                             modifier.condition,
                             failure)
            || !tables::read(blob, offset + layout::ModifierRow::kValueIndex, modifier.valueIndex)
            || !tables::read(blob, offset + layout::ModifierRow::kValue, modifier.value)
            || !std::isfinite(modifier.value)) {
            reason = failure == BankFailure::full ? "instruction_capacity" : "instruction_storage";
            return false;
        }
        if (modifiers_.size() >= domain::kModifierCapacity) {
            failure = BankFailure::full;
            reason = "modifier_capacity";
            return false;
        }
        if (!append(modifiers_, modifier, domain::kModifierCapacity)) {
            failure = BankFailure::storage;
            reason = "modifier_storage";
            return false;
        }
    }
    std::array<domain::SocketOverride, domain::kSocketsPerItem> overrides{};
    std::size_t count = 0;
    if (!read_reward_sockets(blob, at + layout::EntryRow::kSockets, overrides, count)) {
        return false;
    }
    if (count > domain::kSocketOverrideCapacity - sockets_.size()) {
        failure = BankFailure::full;
        reason = "socket_capacity";
        return false;
    }
    out.sockets = {static_cast<std::uint32_t>(sockets_.size()), static_cast<std::uint32_t>(count)};
    for (std::size_t i = 0; i < count; ++i) {
        if (overrides[i].socketType == domain::kAbsent) {
            return false;
        }
        if (!append(sockets_, overrides[i], domain::kSocketOverrideCapacity)) {
            failure = BankFailure::storage;
            reason = "socket_storage";
            return false;
        }
    }
    if (entries_.size() >= domain::kEntryCapacity) {
        failure = BankFailure::full;
        reason = "entry_capacity";
        return false;
    }
    if (!append(entries_, out, domain::kEntryCapacity)) {
        failure = BankFailure::storage;
        reason = "entry_storage";
        return false;
    }
    return true;
}

/**
 * Binds each class to the flag its default finisher's use gate reads.
 * @return False when a table is unreadable or its schema refuses; a refusal settles the domain.
 */
bool RewardConditions::load_class_flags(const reader::Source& source,
                                        reader::Scratch& scratch,
                                        std::span<const std::byte> root) noexcept {
    classFlags_.fill(domain::kAbsent);
    std::vector<std::byte> classes;
    std::vector<std::byte> index;
    std::vector<std::byte> blob;
    tables::Array classRows{};
    tables::Array itemRows{};
    if (!load_reward_table(source,
                           scratch,
                           root,
                           layout::RootSlots::kClasses,
                           classes,
                           layout::ClassRow::kTableClass,
                           "class_table")
        || !load_reward_table(
            source, scratch, root, tables::kItemTableSlot, index, 0, "class_item_table")) {
        return false;
    }
    if (!tables::read_array(classes,
                            offsetof(generated::CharacterClassTable, table),
                            layout::ClassRow::kClass,
                            layout::ClassRow::kSize,
                            classRows)
        || classRows.count != classFlags_.size()
        || !tables::find_array_at(index, offsetof(generated::ItemIndexTable, table), itemRows)
        || itemRows.elementClass != tables::kItemIndexTableClass) {
        settle_unsupported("class_schema");
        return false;
    }
    for (std::size_t i = 0; i < classFlags_.size(); ++i) {
        std::uint16_t itemIndex = domain::kAbsent;
        tables::IndexRow item{};
        std::uint32_t cls = 0;
        if (!tables::read(std::span<const std::byte>{classes},
                          classRows.dataOffset + i * layout::ClassRow::kSize
                              + layout::ClassRow::kDefaultFinisher,
                          itemIndex)
            || !tables::index_row(index, itemRows, itemIndex, item)) {
            settle_unsupported("class_finisher_index");
            return false;
        }
        if (!reader::read_tag(source, scratch, item.targetTag, blob, cls)) {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::warn,
                              "ev=pkg stage=class_flags class=%zu result=fail reason=finisher_read",
                              i);
            return false;
        }
        if (cls != tables::kItemDefinitionClass || !read_class_flag(blob, classFlags_[i])) {
            settle_unsupported("class_finisher_schema");
            return false;
        }
    }
    return true;
}

/**
 * Reads the class bindings and the flag, value and shared expression tables.
 * @param maps Unlock slot maps the binder reads; they must outlive this object's use.
 * @return False when a table is unreadable or refused; a refusal settles the domain.
 */
bool RewardConditions::load(const reader::Source& source,
                            reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept {
    loaded_ = false;
    maps_ = &maps;
    if (!load_class_flags(source, scratch, root)
        || !load_reward_table(source,
                              scratch,
                              root,
                              layout::RootSlots::kFlagSlots,
                              flags_,
                              layout::SlotRow::kFlagTableClass,
                              "flag_table")
        || !load_reward_table(source,
                              scratch,
                              root,
                              layout::RootSlots::kValueSlots,
                              values_,
                              layout::SlotRow::kValueTableClass,
                              "value_table")
        || !load_reward_table(source,
                              scratch,
                              root,
                              layout::RootSlots::kExpressionPool,
                              expressions_,
                              layout::ExpressionPoolRow::kTableClass,
                              "expression_table")) {
        return false;
    }
    // Shared expressions are expanded before the runtime evaluates reward conditions.
    if (!tables::read_array(flags_,
                            offsetof(generated::UnlockFlagTable, table),
                            layout::SlotRow::kFlagClass,
                            layout::SlotRow::kSize,
                            flagRows_)
        || !tables::read_array(values_,
                               offsetof(generated::UnlockValueTable, table),
                               layout::SlotRow::kValueClass,
                               layout::SlotRow::kSize,
                               valueRows_)
        || !tables::read_array(expressions_,
                               offsetof(generated::RewardExpressionTable, table),
                               layout::ExpressionPoolRow::kClass,
                               layout::ExpressionPoolRow::kSize,
                               expressionRows_)) {
        settle_unsupported("condition_schema");
        return false;
    }
    loaded_ = true;
    return true;
}

bool RewardConditions::loaded() const noexcept {
    return loaded_;
}

/**
 * Binds one native instruction to the bank the server evaluates it against.
 * A flag or value slot maps to its saved bank, a class finisher flag to the class bank, and any
 * other slot to an external identity hash.
 * @param native Native opcode word.
 * @param operand Native operand: a slot, a constant or nothing.
 * @param instruction Receives the bound instruction.
 * @return False for an opcode the evaluator refuses or a slot outside its table.
 */
bool RewardConditions::bind(std::uint32_t native,
                            std::uint32_t operand,
                            domain::Instruction& instruction) const noexcept {
    const bool flag = native == tables::kUnlockReadFlagOpcode;
    if (!flag && native != tables::kUnlockReadValueOpcode) {
        instruction = {{}, unlocks::Bank::none, operand};
        return unlocks::decode_opcode(native, instruction.opcode);
    }
    const auto& rows = flag ? flagRows_ : valueRows_;
    const std::span<const std::byte> blob = flag ? flags_ : values_;
    std::uint32_t hash = 0;
    if (operand >= rows.count
        || !tables::read(blob,
                         rows.dataOffset + operand * layout::SlotRow::kSize
                             + layout::SlotRow::kHash,
                         hash)) {
        return false;
    }
    instruction.opcode = flag ? unlocks::Opcode::flag : unlocks::Opcode::loadValue;
    if (flag) {
        for (std::size_t characterClass = 0; characterClass < classFlags_.size();
             ++characterClass) {
            if (classFlags_[characterClass] != domain::kAbsent
                && operand == classFlags_[characterClass]) {
                instruction.bank = unlocks::Bank::characterClass;
                instruction.operand = static_cast<std::uint32_t>(characterClass);
                return true;
            }
        }
    }
    const auto slot = static_cast<std::int32_t>(operand);
    const auto accountIndex = bank_index(flag ? maps_->accountFlag : maps_->accountValue, slot);
    const auto profileIndex = flag ? bank_index(maps_->profileFlag, slot) : kUnmappedSlot;
    const auto characterIndex =
        bank_index(flag ? maps_->characterFlag : maps_->characterValue, slot);
    if (accountIndex != kUnmappedSlot) {
        instruction.bank = unlocks::Bank::account;
        instruction.operand = accountIndex;
    } else if (profileIndex != kUnmappedSlot) {
        instruction.bank = unlocks::Bank::profile;
        instruction.operand = profileIndex;
    } else if (characterIndex != kUnmappedSlot) {
        instruction.bank = unlocks::Bank::character;
        instruction.operand = characterIndex;
    } else {
        instruction.bank = unlocks::Bank::external;
        instruction.operand = hash;
    }
    return true;
}

/**
 * Appends one expression, expanding each shared-pool reference in place.
 * @param at Offset of the expression field.
 * @param depth Shared-pool nesting so far; it is bounded by the traversal depth.
 * @param failure Receives full or storage when the bank refused.
 * @return False for an unreadable or unbindable instruction or a refused bank.
 */
bool RewardConditions::append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<domain::Instruction>& bank,
                                         std::size_t depth,
                                         BankFailure& failure) const noexcept {
    tables::Array rows{};
    if (depth >= domain::kTraversalDepth
        || !tables::read_array(blob,
                               at,
                               tables::kInvestmentExpressionRowClass,
                               tables::kUnlockInstructionStride,
                               rows)) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        const std::size_t row = rows.dataOffset + i * tables::kUnlockInstructionStride;
        std::uint32_t native = 0;
        std::uint32_t operand = 0;
        if (!tables::read(blob, row + tables::kUnlockInstructionOpcodeOffset, native)
            || !tables::read(blob, row + tables::kUnlockInstructionOperandOffset, operand)) {
            return false;
        }
        if (native == tables::kUnlockExpressionOpcode) {
            const auto before = bank.size();
            if (operand >= expressionRows_.count
                || !append_expression(expressions_,
                                      expressionRows_.dataOffset
                                          + operand * layout::ExpressionPoolRow::kSize
                                          + layout::ExpressionPoolRow::kExpression,
                                      bank,
                                      depth + 1,
                                      failure)
                || bank.size() == before) {
                return false;
            }
            continue;
        }
        domain::Instruction instruction{};
        if (!bind(native, operand, instruction) || !unlocks::valid(instruction)) {
            return false;
        }
        if (bank.size() >= domain::kInstructionCapacity) {
            failure = BankFailure::full;
            return false;
        }
        if (!append(bank, instruction, domain::kInstructionCapacity)) {
            failure = BankFailure::storage;
            return false;
        }
    }
    return true;
}

/**
 * Appends one expanded expression to a bank.
 * @param range Receives the appended rows.
 * @param failure Receives full or storage when the bank refused.
 * @return False when the expression does not expand; the bank is unchanged then.
 */
bool RewardConditions::read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<domain::Instruction>& bank,
                            domain::Range& range,
                            BankFailure& failure) const noexcept {
    const auto first = bank.size();
    failure = BankFailure::none;
    if (!append_expression(blob, at, bank, 0, failure)) {
        bank.resize(first);
        return false;
    }
    range = {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bank.size() - first)};
    return true;
}

/**
 * Expands a list of expressions into one program where every expression must hold.
 * @param output Receives the program; it bounds the expanded length.
 * @param count Receives the program length; zero for an empty list.
 * @param failure Receives storage when a bank allocation failed.
 * @param rowClass Element class of the list; each row is one expression field.
 * @return False for an unreadable, empty or oversized expression.
 */
bool RewardConditions::read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<domain::Instruction> output,
                                 std::size_t& count,
                                 BankFailure& failure,
                                 std::uint32_t rowClass) const noexcept {
    count = 0;
    failure = BankFailure::none;
    tables::Array rows{};
    std::vector<domain::Instruction> instructions;
    if (!tables::read_array(blob, at, rowClass, layout::ConditionRow::kSize, rows)) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        domain::Range expression{};
        if (!read(blob,
                  rows.dataOffset + i * layout::ConditionRow::kSize,
                  instructions,
                  expression,
                  failure)
            || expression.count == 0) {
            return false;
        }
        // Every expression attached to a progression reward must hold.
        if (i != 0) {
            if (instructions.size() >= output.size()) {
                return false;
            }
            if (!append(instructions,
                        domain::Instruction{unlocks::Opcode::logicalAnd, unlocks::Bank::none, 0},
                        output.size())) {
                failure = BankFailure::storage;
                return false;
            }
        }
        if (instructions.size() > output.size()) {
            return false;
        }
    }
    std::copy(instructions.begin(), instructions.end(), output.begin());
    count = instructions.size();
    return true;
}

/**
 * Expands the shared expression a row index names.
 * @param index Row of the shared expression table.
 * @param output Receives the program; it bounds the expanded length.
 * @param count Receives the program length.
 * @param failure Receives storage when a bank allocation failed.
 * @return False for a row outside the table or an unreadable, empty or oversized expression.
 */
bool RewardConditions::read_index(std::uint16_t index,
                                  std::span<domain::Instruction> output,
                                  std::size_t& count,
                                  BankFailure& failure) const noexcept {
    count = 0;
    failure = BankFailure::none;
    std::vector<domain::Instruction> instructions;
    domain::Range expression{};
    if (!loaded_ || index >= expressionRows_.count
        || !read(expressions_,
                 expressionRows_.dataOffset + index * layout::ExpressionPoolRow::kSize
                     + layout::ExpressionPoolRow::kExpression,
                 instructions,
                 expression,
                 failure)
        || expression.count == 0 || expression.count > output.size()) {
        return false;
    }
    std::copy(instructions.begin(), instructions.end(), output.begin());
    count = instructions.size();
    return true;
}

/**
 * Names the value slot an expression reads from the held item.
 * @param at Offset of the expression field.
 * @param mode Receives which held instances the client reads the slot from.
 * @return The slot when the expression is one read of an item-held value, otherwise
 *         objectives::kNoItemValueSlot.
 */
std::uint16_t RewardConditions::item_value_slot(
    std::span<const std::byte> blob,
    std::size_t at,
    state::build_data::objectives::ItemValueMode& mode) const noexcept {
    using Mode = state::build_data::objectives::ItemValueMode;
    constexpr std::uint16_t kNone = state::build_data::objectives::kNoItemValueSlot;
    mode = Mode::context;
    tables::Array rows{};
    std::uint32_t slot = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    if (!loaded_ || !tables::find_array_at(blob, at, rows)
        || !tables::expression_lone_operand(blob, rows, tables::kUnlockReadValueOpcode, slot)
        || slot >= valueRows_.count || slot >= kNone) {
        return kNone;
    }
    const std::span<const std::byte> bindings{values_};
    const std::size_t binding = valueRows_.dataOffset + slot * layout::SlotRow::kSize;
    if (!tables::read(bindings, binding + layout::SlotRow::kValueKind, kind)
        || !tables::read(bindings, binding + layout::SlotRow::kValueFlags, flags)
        || kind != layout::SlotRow::kItemValueKind
        || (flags != 0 && flags != layout::SlotRow::kItemValueIgnoredFlag
            && flags != layout::SlotRow::kItemValueMaximumFlag)) {
        return kNone;
    }
    if (flags == layout::SlotRow::kItemValueMaximumFlag) {
        mode = Mode::maximum;
    }
    return static_cast<std::uint16_t>(slot);
}

/** Reads a socket override array; false when unreadable or wider than the output. */
bool read_reward_sockets(std::span<const std::byte> blob,
                         std::size_t at,
                         std::span<domain::SocketOverride> output,
                         std::size_t& count) noexcept {
    using Row = layout::SocketOverrideRow;
    count = 0;
    tables::Array rows{};
    if (!tables::read_array(blob, at, Row::kClass, Row::kSize, rows)
        || rows.count > output.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        const std::size_t p = rows.dataOffset + i * Row::kSize;
        auto& socket = output[i];
        if (!tables::read(blob, p + Row::kSocketType, socket.socketType)
            || !tables::read(blob, p + Row::kPlugItem, socket.plugItem)
            || !tables::read(blob, p + Row::kPlugSet, socket.plugSet)
            || !tables::read(blob, p + Row::kRollSet, socket.rollSet)
            || !tables::read(blob, p + Row::kSelection, socket.selection)) {
            return false;
        }
    }
    count = static_cast<std::size_t>(rows.count);
    return true;
}

/**
 * Loads every reward pool and its entries, which bind through the loaded condition tables.
 * An unreadable entry or pool is skipped; a full bank or a refused schema settles the domain.
 * @return False when the conditions are not loaded or a table or bank refused; conditions that
 *         loaded still serve the season pass.
 */
bool RewardBuild::load(const reader::Source& source,
                       reader::Scratch& scratch,
                       std::span<const std::byte> root) noexcept {
    loaded_ = false;
    supplementalMissing_ = false;
    pools_.clear();
    entries_.clear();
    instructions_.clear();
    modifiers_.clear();
    sockets_.clear();
    std::vector<std::byte> blob;
    tables::Array rows{};
    std::uint32_t supplementalTag = 0;
    if (!conditions.loaded()) {
        return false;
    }
    if (!tables::slot_tag(root, layout::RootSlots::kSupplementalRewards, supplementalTag)) {
        settle_unsupported("supplemental_slot");
        return false;
    }
    if (!load_reward_table(source,
                           scratch,
                           root,
                           layout::RootSlots::kPools,
                           blob,
                           layout::PoolRow::kTableClass,
                           "pool_table")) {
        return false;
    }
    if (!tables::read_array(blob,
                            offsetof(generated::RewardPoolTable, table),
                            layout::PoolRow::kClass,
                            layout::PoolRow::kSize,
                            rows)) {
        settle_unsupported("pool_schema");
        return false;
    }
    if (rows.count == 0 || rows.count > domain::kPoolCapacity) {
        settle_unsupported("pool_count");
        return false;
    }
    supplementalMissing_ = supplementalTag == layout::kAbsentTableTag;
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto at = rows.dataOffset + i * layout::PoolRow::kSize;
        domain::Pool pool{};
        tables::Array members{};
        if (tables::read(blob, at + layout::PoolRow::kHash, pool.definitionHash)
            && pool.definitionHash != 0
            && tables::read_array(blob,
                                  at + layout::PoolRow::kEntries,
                                  layout::EntryRow::kClass,
                                  layout::EntryRow::kSize,
                                  members)) {
            pool.entries.first = static_cast<std::uint32_t>(entries_.size());
            for (std::size_t j = 0; j < members.count; ++j) {
                const auto beforeInstructions = instructions_.size();
                const auto beforeModifiers = modifiers_.size();
                const auto beforeSockets = sockets_.size();
                auto failure = BankFailure::none;
                const char* reason = nullptr;
                if (!entry(
                        blob, members.dataOffset + j * layout::EntryRow::kSize, failure, reason)) {
                    // Skipping a bank failure would publish a pool missing some of its entries.
                    if (failure == BankFailure::full) {
                        settle_unsupported(reason);
                        return false;
                    }
                    if (failure == BankFailure::storage) {
                        report_refusal(reason);
                        return false;
                    }
                    instructions_.resize(beforeInstructions);
                    modifiers_.resize(beforeModifiers);
                    sockets_.resize(beforeSockets);
                    ++skipped;
                }
            }
            pool.entries.count = static_cast<std::uint32_t>(entries_.size()) - pool.entries.first;
        } else {
            pool = {};
            ++skipped;
        }
        if (!append(pools_, pool, domain::kPoolCapacity)) {
            report_refusal("pool_storage");
            return false;
        }
    }
    core::log::writef(core::log::Channel::client,
                      skipped == 0 ? core::log::Level::info : core::log::Level::warn,
                      "ev=pkg stage=rewards pools=%zu entries=%zu instructions=%zu modifiers=%zu "
                      "sockets=%zu skipped=%zu",
                      pools_.size(),
                      entries_.size(),
                      instructions_.size(),
                      modifiers_.size(),
                      sockets_.size(),
                      skipped);
    loaded_ = true;
    return true;
}

/**
 * Reads one item's acquisition flag and, for a wrapper, its pool and category selections.
 * An acquisition flag that binds outside the account bank is counted and dropped.
 * @param hash Item definition hash the row keeps.
 * @param blob Item definition bytes.
 * @param item Receives the row.
 * @return False when a field or the wrapper block is unreadable.
 */
bool RewardBuild::read_item(std::uint32_t hash,
                            std::span<const std::byte> blob,
                            domain::Item& item) noexcept {
    std::uint16_t acquired = domain::kAbsent;
    std::int64_t wrapper = 0;
    std::uint32_t cls = 0;
    if (!tables::read(blob, layout::ItemFields::kWrapper, wrapper)
        || !tables::read(blob, layout::ItemFields::kAcquiredFlag, acquired)) {
        return false;
    }
    item.definitionHash = hash;
    if (acquired != domain::kAbsent) {
        domain::Instruction flag{};
        if (!conditions.bind(tables::kUnlockReadFlagOpcode, acquired, flag)
            || !unlocks::valid(flag)) {
            // The grant is kept; only its acquisition flag is unknown.
            ++unboundAcquiredFlags_;
        } else if (flag.bank == unlocks::Bank::account) {
            item.acquiredFlag = static_cast<std::uint16_t>(flag.operand);
        } else {
            ++discardedAcquiredFlags_;
        }
    }
    // A zero delta means the item is not a wrapper.
    if (wrapper != 0) {
        std::size_t at = 0;
        tables::Array selections{};
        if (!tables::relative(blob, layout::ItemFields::kWrapper, at)
            || !reader::layout::class_word_before(blob, at, cls)
            || cls != layout::WrapperBlock::kClass
            || !tables::read(blob, at + layout::WrapperBlock::kPool, item.poolIndex)
            || !tables::read(blob, at + layout::WrapperBlock::kFlags, item.flags)
            || !tables::read_array(blob,
                                   at + layout::WrapperBlock::kSelections,
                                   layout::SelectionRow::kClass,
                                   layout::SelectionRow::kSize,
                                   selections)
            || selections.count > item.selections.size()) {
            return false;
        }
        item.selectionCount = static_cast<std::uint8_t>(selections.count);
        for (std::size_t j = 0; j < selections.count; ++j) {
            auto& selection = item.selections[j];
            const auto p = selections.dataOffset + j * layout::SelectionRow::kSize;
            if (!tables::read(blob, p + layout::SelectionRow::kCategory, selection.categoryHash)
                || !tables::read(blob, p + layout::SelectionRow::kCount, selection.count)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Sizes the dense item rows before the item walk and resets the walk's counters.
 * @param count Rows in the installed item table.
 * @return False after a refused load or when the rows do not fit.
 */
bool RewardBuild::begin_items(std::size_t count) noexcept {
    unboundAcquiredFlags_ = 0;
    discardedAcquiredFlags_ = 0;
    unreadItems_ = 0;
    // A refused load has already reported itself.
    if (!loaded_) {
        return false;
    }
    try {
        if (count == 0 || count > domain::kItemCapacity) {
            report_refusal("item_storage");
            return false;
        }
        items_.assign(count, {});
        return true;
    } catch (...) {
        report_refusal("item_storage");
        return false;
    }
}

void RewardBuild::item(std::uint16_t index,
                       std::uint32_t hash,
                       std::span<const std::byte> blob) noexcept {
    domain::Item parsed{};
    if (index < items_.size() && read_item(hash, blob, parsed)) {
        items_[index] = parsed;
    }
}

void RewardBuild::item_unread(std::uint16_t index) noexcept {
    if (unreadItems_++ == 0) {
        firstUnreadItem_ = index;
    }
}

/**
 * Trims item rows to the catalog, drops entries naming unread items or pools, then publishes.
 * @param catalogItemCount Rows the published item catalog holds.
 * @return False after a refused load, an unclean item walk, a refused graph or a failed write.
 */
bool RewardBuild::publish(std::size_t catalogItemCount) noexcept {
    if (!loaded_) {
        return false;
    }
    // Publishing now would prune every entry that names an unread item.
    if (unreadItems_ != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=rewards result=fail reason=item_read unread_items=%zu "
                          "first_index=%u",
                          unreadItems_,
                          static_cast<unsigned>(firstUnreadItem_));
        return false;
    }
    // A catalog that lost trailing rows is shorter than the table, and the cache pairs each of its
    // items with one reward row.
    if (catalogItemCount < items_.size()) {
        items_.resize(catalogItemCount);
    }
    // Cleared first so the entry pass also drops draws of a cleared wrapper.
    for (auto& item : items_) {
        if (item.poolIndex != domain::kAbsent && item.poolIndex >= pools_.size()) {
            item = {};
        }
    }
    std::size_t write = 0;
    std::size_t socketWrite = 0;
    for (auto& pool : pools_) {
        const auto range = pool.entries;
        pool.entries.first = static_cast<std::uint32_t>(write);
        for (std::size_t i = range.first; i < range.first + range.count; ++i) {
            const auto& row = entries_[i];
            bool valid = (row.itemIndex == domain::kAbsent || item_read(items_, row.itemIndex))
                         && (row.poolIndex == domain::kAbsent || row.poolIndex < pools_.size());
            for (const auto& socket :
                 std::span(sockets_).subspan(row.sockets.first, row.sockets.count)) {
                valid &=
                    domain::valid_socket(socket, items_.size())
                    && (socket.plugItem == domain::kAbsent || item_read(items_, socket.plugItem));
            }
            if (valid) {
                auto retained = row;
                retained.sockets.first = static_cast<std::uint32_t>(socketWrite);
                for (const auto& socket :
                     std::span(sockets_).subspan(row.sockets.first, row.sockets.count)) {
                    sockets_[socketWrite++] = socket;
                }
                entries_[write++] = retained;
            }
        }
        pool.entries.count = static_cast<std::uint32_t>(write) - pool.entries.first;
    }
    const std::size_t droppedEntries = entries_.size() - write;
    entries_.resize(write);
    sockets_.resize(socketWrite);
    if (unboundAcquiredFlags_ != 0 || discardedAcquiredFlags_ != 0 || droppedEntries != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=rewards result=partial unbound_acquired_flags=%zu "
                          "discarded_acquired_flags=%zu dropped_entries=%zu",
                          unboundAcquiredFlags_,
                          discardedAcquiredFlags_,
                          droppedEntries);
    }
    const domain::View graph{pools_, entries_, items_, instructions_, modifiers_, sockets_};
    if (!state::build_data::valid_reward_definitions(graph)) {
        settle_unsupported("graph");
        return false;
    }
    // A cache write can still fail, so this stays retryable.
    if (!state::build_data::publish_reward_definitions(graph)) {
        report_refusal("publish");
        return false;
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
