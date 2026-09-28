#include "package_reward_build.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/field_reader.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/unlocks/unlocks_expression.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::rewards;
namespace unlocks = state::unlocks;

/** Investment-root slots identify reward pools and unlock-slot bindings. */
constexpr std::size_t kSupplementalRewardSlot = 84;
constexpr std::size_t kPoolSlot = 88;
constexpr std::size_t kExpressionSlot = 109;
constexpr std::size_t kFlagSlot = 112;
constexpr std::size_t kValueSlot = 114;
/** Only the omitted-bank sentinel permits a no-op; other invalid tags leave effects unresolved. */
constexpr std::uint32_t kAbsentTableTag = 0xFFFFFFFFU;
/** Native reward schema classes and fixed row sizes. */
constexpr std::uint32_t kPoolClass = 0x80807553U;
constexpr std::uint32_t kPoolRowClass = 0x8080748CU;
constexpr std::uint32_t kEntryClass = 0x8080748EU;
constexpr std::uint32_t kExpressionClass = 0x80807D31U;
constexpr std::uint32_t kModifierClass = 0x80807490U;
constexpr std::uint32_t kSocketClass = 0x80803062U;
constexpr std::uint32_t kWrapperClass = 0x808077CCU;
constexpr std::uint32_t kSelectionClass = 0x808077CFU;
constexpr std::uint32_t kFlagTableClass = 0x80807D49U;
constexpr std::uint32_t kFlagRowClass = 0x80807D4FU;
constexpr std::uint32_t kValueTableClass = 0x80807C92U;
constexpr std::uint32_t kValueRowClass = 0x80807C96U;
constexpr std::uint32_t kExpressionTableClass = 0x80807C49U;
constexpr std::uint32_t kExpressionRowClass = 0x80807C4FU;
constexpr std::uint32_t kConditionClass = 0x80807D2FU;
constexpr std::size_t kPoolStride = 24;
constexpr std::size_t kEntryStride = 80;
constexpr std::size_t kModifierStride = 24;
constexpr std::size_t kBindingStride = 8;
constexpr std::size_t kExpressionRowStride = 24;
constexpr std::size_t kSocketStride = 12;
constexpr std::size_t kSelectionStride = 12;
/** Item headers hold a relative wrapper pointer and an acquired-unlock slot. */
constexpr std::size_t kWrapperField = 0x58;
constexpr std::size_t kAcquiredFlagField = 0xDA;

/** Entry +10 selects a supplemental reward, not an item or nested pool. */
constexpr std::size_t kEntryQuantityOffset = 4;
constexpr std::size_t kEntryPoolOffset = 8;
constexpr std::size_t kEntrySupplementalOffset = 10;
constexpr std::size_t kEntryCategoryOffset = 20;
constexpr std::size_t kEntryWeightOffset = 24;
constexpr std::size_t kEntryConditionOffset = 32;
constexpr std::size_t kEntryModifiersOffset = 48;
constexpr std::size_t kEntrySocketsOffset = 64;
constexpr std::size_t kModifierValueIndexOffset = 16;
constexpr std::size_t kModifierValueOffset = 20;
constexpr std::size_t kPoolEntriesOffset = 8;
constexpr std::size_t kExpressionBodyOffset = 8;
constexpr std::size_t kWrapperSelectionsOffset = 8;
constexpr std::size_t kWrapperFlagsOffset = 24;
constexpr std::size_t kSelectionCountOffset = 4;
constexpr std::size_t kSocketPlugOffset = 2;
constexpr std::size_t kSocketPlugSetOffset = 4;
constexpr std::size_t kSocketRollSetOffset = 6;
constexpr std::size_t kSocketSelectionOffset = 8;
/** Relative definition bodies are preceded by their four-byte schema class. */
constexpr std::size_t kDefinitionClassPrefixSize = sizeof(std::uint32_t);

/** Class rows name a default finisher whose use gate supplies the computed class flag. */
constexpr std::size_t kClassSlot = 12;
constexpr std::size_t kClassStride = 32;
constexpr std::size_t kDefaultFinisherOffset = 24;
constexpr std::uint32_t kClassTable = 0x808075BEU;
constexpr std::uint32_t kClassRow = 0x808074FAU;
constexpr std::size_t kUseConditionPointer = 24;
constexpr std::uint32_t kUseConditionClass = 0x80802980U;

/** Set when the installed data refuses the reward tables or graph; no retry can change that. */
bool g_rewardsUnsupported = false;

/** Logs one refused reward load, sizing or publication. */
void report_refusal(const char* reason) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=rewards result=fail reason=%s",
                      reason);
}

/** Logs a refusal the installed data decides and settles the domain once. */
void settle_unsupported(const char* reason) noexcept {
    report_refusal(reason);
    if (!g_rewardsUnsupported) {
        g_rewardsUnsupported = true;
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

/** Reads the table an investment-root slot names, checking its class when one is expected. */
bool root_table(const reader::Source& source,
                reader::Scratch& scratch,
                std::span<const std::byte> root,
                std::size_t slot,
                std::vector<std::byte>& blob,
                std::uint32_t expectedClass = 0) noexcept {
    std::uint32_t tag = 0;
    std::uint32_t cls = 0;
    return tables::slot_tag(root, slot, tag) && tables::package_of(tag) != tables::kAbsentPackageId
           && reader::read_tag(source, scratch, tag, blob, cls)
           && (expectedClass == 0 || cls == expectedClass);
}

/** Reads one entry's fixed fields; a negative or non-finite weight is refused. */
bool read_reward_entry(std::span<const std::byte> blob,
                       std::size_t at,
                       domain::Entry& entry) noexcept {
    return tables::read(blob, at, entry.itemIndex)
           && tables::read(blob, at + kEntryQuantityOffset, entry.quantity)
           && tables::read(blob, at + kEntryPoolOffset, entry.poolIndex)
           && tables::read(blob, at + kEntrySupplementalOffset, entry.supplementalIndex)
           && tables::read(blob, at + kEntryCategoryOffset, entry.categoryHash)
           && tables::read(blob, at + kEntryWeightOffset, entry.weight)
           && std::isfinite(entry.weight) && entry.weight >= 0;
}

/** Reads the single flag that gates use of the class's default finisher. */
bool read_class_flag(std::span<const std::byte> blob, std::uint16_t& output) noexcept {
    std::size_t at = 0;
    std::uint32_t cls = 0;
    tables::Array expression{};
    if (!tables::relative(blob, kUseConditionPointer, at) || at < kDefinitionClassPrefixSize
        || !tables::read(blob, at - kDefinitionClassPrefixSize, cls) || cls != kUseConditionClass
        || !tables::read_array(
            blob, at, kExpressionClass, tables::kUnlockInstructionStride, expression)
        || expression.count != 1) {
        return false;
    }
    std::uint32_t opcode = 0;
    std::uint32_t flag = domain::kAbsent;
    if (!tables::read(blob, expression.dataOffset, opcode)
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
    return state::build_data::reward_definitions_ready() || g_rewardsUnsupported;
}

bool RewardBuild::entry(std::span<const std::byte> blob,
                        std::size_t at,
                        const char*& fullBank) noexcept {
    domain::Entry out{};
    bool instructionsFull = false;
    if (!read_reward_entry(blob, at, out)
        || !conditions.read(
            blob, at + kEntryConditionOffset, instructions_, out.condition, instructionsFull)) {
        fullBank = instructionsFull ? "instruction_capacity" : nullptr;
        return false;
    }
    out.supplementalMissing = supplementalMissing_ && out.supplementalIndex != domain::kAbsent;
    tables::Array rows{};
    if (!tables::read_array(
            blob, at + kEntryModifiersOffset, kModifierClass, kModifierStride, rows)) {
        return false;
    }
    out.modifiers = {static_cast<std::uint32_t>(modifiers_.size()),
                     static_cast<std::uint32_t>(rows.count)};
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto offset = rows.dataOffset + i * kModifierStride;
        domain::Modifier modifier{};
        if (!conditions.read(blob, offset, instructions_, modifier.condition, instructionsFull)
            || !tables::read(blob, offset + kModifierValueIndexOffset, modifier.valueIndex)
            || !tables::read(blob, offset + kModifierValueOffset, modifier.value)
            || !std::isfinite(modifier.value)) {
            fullBank = instructionsFull ? "instruction_capacity" : nullptr;
            return false;
        }
        if (modifiers_.size() >= domain::kModifierCapacity) {
            fullBank = "modifier_capacity";
            return false;
        }
        if (!append(modifiers_, modifier, domain::kModifierCapacity)) {
            return false;
        }
    }
    std::array<domain::SocketOverride, domain::kSocketsPerItem> overrides{};
    std::size_t count = 0;
    if (!read_reward_sockets(blob, at + kEntrySocketsOffset, overrides, count)) {
        return false;
    }
    if (count > domain::kSocketOverrideCapacity - sockets_.size()) {
        fullBank = "socket_capacity";
        return false;
    }
    out.sockets = {static_cast<std::uint32_t>(sockets_.size()), static_cast<std::uint32_t>(count)};
    for (std::size_t i = 0; i < count; ++i) {
        if (overrides[i].socketType == domain::kAbsent
            || !append(sockets_, overrides[i], domain::kSocketOverrideCapacity)) {
            return false;
        }
    }
    if (entries_.size() >= domain::kEntryCapacity) {
        fullBank = "entry_capacity";
        return false;
    }
    return append(entries_, out, domain::kEntryCapacity);
}

void RewardConditions::load_class_flags(const reader::Source& source,
                                        reader::Scratch& scratch,
                                        std::span<const std::byte> root) noexcept {
    classFlags_.fill(domain::kAbsent);
    std::vector<std::byte> classes;
    std::vector<std::byte> index;
    std::vector<std::byte> blob;
    tables::Array classRows{};
    tables::Array itemRows{};
    if (!root_table(source, scratch, root, kClassSlot, classes, kClassTable)
        || !tables::read_array(
            classes, tables::kTableArrayDescriptor, kClassRow, kClassStride, classRows)
        || classRows.count != classFlags_.size()
        || !root_table(source, scratch, root, tables::kItemTableSlot, index)
        || !tables::read_array(index,
                               tables::kTableArrayDescriptor,
                               tables::kItemIndexTableClass,
                               tables::kItemIndexRowStride,
                               itemRows)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=pkg stage=class_flags result=skip reason=class_tables");
        return;
    }
    for (std::size_t i = 0; i < classFlags_.size(); ++i) {
        std::uint16_t itemIndex = domain::kAbsent;
        tables::IndexRow item{};
        std::uint32_t cls = 0;
        if (!tables::read(std::span<const std::byte>{classes},
                          classRows.dataOffset + i * kClassStride + kDefaultFinisherOffset,
                          itemIndex)
            || !tables::index_row(index, itemRows, itemIndex, item)
            || !reader::read_tag(source, scratch, item.targetTag, blob, cls)
            || cls != tables::kItemDefinitionClass) {
            core::log::writef(
                core::log::Channel::client,
                core::log::Level::warn,
                "ev=pkg stage=class_flags class=%zu result=skip reason=default_finisher",
                i);
            continue;
        }
        if (!read_class_flag(blob, classFlags_[i])) {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::warn,
                              "ev=pkg stage=class_flags class=%zu result=skip reason=use_condition",
                              i);
        }
    }
}

bool RewardConditions::load(const reader::Source& source,
                            reader::Scratch& scratch,
                            std::span<const std::byte> root,
                            const SlotMaps& maps) noexcept {
    maps_ = &maps;
    load_class_flags(source, scratch, root);
    // Shared expressions are expanded before the runtime evaluates reward conditions.
    loaded_ =
        root_table(source, scratch, root, kFlagSlot, flags_, kFlagTableClass)
        && tables::read_array(
            flags_, tables::kTableArrayDescriptor, kFlagRowClass, kBindingStride, flagRows_)
        && root_table(source, scratch, root, kValueSlot, values_, kValueTableClass)
        && tables::read_array(
            values_, tables::kTableArrayDescriptor, kValueRowClass, kBindingStride, valueRows_)
        && root_table(source, scratch, root, kExpressionSlot, expressions_, kExpressionTableClass)
        && tables::read_array(expressions_,
                              tables::kTableArrayDescriptor,
                              kExpressionRowClass,
                              kExpressionRowStride,
                              expressionRows_);
    return loaded_;
}

bool RewardConditions::loaded() const noexcept {
    return loaded_;
}

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
        || !tables::read(blob, rows.dataOffset + operand * kBindingStride, hash)) {
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

bool RewardConditions::append_expression(std::span<const std::byte> blob,
                                         std::size_t at,
                                         std::vector<domain::Instruction>& bank,
                                         std::size_t depth) const noexcept {
    tables::Array rows{};
    if (depth >= domain::kTraversalDepth
        || !tables::read_array(
            blob, at, kExpressionClass, tables::kUnlockInstructionStride, rows)) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        const std::size_t row = rows.dataOffset + i * tables::kUnlockInstructionStride;
        std::uint32_t native = 0;
        std::uint32_t operand = 0;
        if (!tables::read(blob, row, native)
            || !tables::read(blob, row + tables::kUnlockInstructionOperandOffset, operand)) {
            return false;
        }
        if (native == tables::kUnlockExpressionOpcode) {
            const auto before = bank.size();
            if (operand >= expressionRows_.count
                || !append_expression(expressions_,
                                      expressionRows_.dataOffset + operand * kExpressionRowStride
                                          + kExpressionBodyOffset,
                                      bank,
                                      depth + 1)
                || bank.size() == before) {
                return false;
            }
            continue;
        }
        domain::Instruction instruction{};
        if (!bind(native, operand, instruction) || !unlocks::valid(instruction)
            || !append(bank, instruction, domain::kInstructionCapacity)) {
            return false;
        }
    }
    return true;
}

bool RewardConditions::read(std::span<const std::byte> blob,
                            std::size_t at,
                            std::vector<domain::Instruction>& bank,
                            domain::Range& range,
                            bool& bankFull) const noexcept {
    const auto first = bank.size();
    bankFull = false;
    if (!append_expression(blob, at, bank, 0)) {
        bankFull = bank.size() >= domain::kInstructionCapacity;
        bank.resize(first);
        return false;
    }
    range = {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(bank.size() - first)};
    return true;
}

bool RewardConditions::read_list(std::span<const std::byte> blob,
                                 std::size_t at,
                                 std::span<domain::Instruction> output,
                                 std::size_t& count) const noexcept {
    count = 0;
    tables::Array rows{};
    std::vector<domain::Instruction> instructions;
    if (!tables::read_array(blob, at, kConditionClass, tables::kUnlockExpressionFieldSize, rows)) {
        return false;
    }
    // The row's condition capacity below bounds this bank.
    bool bankFull = false;
    for (std::size_t i = 0; i < rows.count; ++i) {
        domain::Range expression{};
        if (!read(blob,
                  rows.dataOffset + i * tables::kUnlockExpressionFieldSize,
                  instructions,
                  expression,
                  bankFull)
            || expression.count == 0) {
            return false;
        }
        // Every expression attached to a progression reward must hold.
        if (i != 0
            && !append(instructions,
                       domain::Instruction{unlocks::Opcode::logicalAnd, unlocks::Bank::none, 0},
                       output.size())) {
            return false;
        }
        if (instructions.size() > output.size()) {
            return false;
        }
    }
    std::copy(instructions.begin(), instructions.end(), output.begin());
    count = instructions.size();
    return true;
}

bool read_reward_sockets(std::span<const std::byte> blob,
                         std::size_t at,
                         std::span<domain::SocketOverride> output,
                         std::size_t& count) noexcept {
    count = 0;
    tables::Array rows{};
    if (!tables::read_array(blob, at, kSocketClass, kSocketStride, rows)
        || rows.count > output.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rows.count; ++i) {
        const std::size_t p = rows.dataOffset + i * kSocketStride;
        auto& socket = output[i];
        if (!tables::read(blob, p, socket.socketType)
            || !tables::read(blob, p + kSocketPlugOffset, socket.plugItem)
            || !tables::read(blob, p + kSocketPlugSetOffset, socket.plugSet)
            || !tables::read(blob, p + kSocketRollSetOffset, socket.rollSet)
            || !tables::read(blob, p + kSocketSelectionOffset, socket.selection)) {
            return false;
        }
    }
    count = static_cast<std::size_t>(rows.count);
    return true;
}

bool RewardBuild::load(const reader::Source& source,
                       reader::Scratch& scratch,
                       std::span<const std::byte> root,
                       const SlotMaps& maps) noexcept {
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
    // Conditions load first; the season pass binds through them even if the pools are refused.
    if (!conditions.load(source, scratch, root, maps)
        || !tables::slot_tag(root, kSupplementalRewardSlot, supplementalTag)
        || !root_table(source, scratch, root, kPoolSlot, blob, kPoolClass)
        || !tables::read_array(
            blob, tables::kTableArrayDescriptor, kPoolRowClass, kPoolStride, rows)) {
        settle_unsupported("tables");
        return false;
    }
    if (rows.count == 0 || rows.count > domain::kPoolCapacity) {
        settle_unsupported("pool_count");
        return false;
    }
    supplementalMissing_ = supplementalTag == kAbsentTableTag;
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < rows.count; ++i) {
        const auto at = rows.dataOffset + i * kPoolStride;
        domain::Pool pool{};
        tables::Array members{};
        if (tables::read(blob, at, pool.definitionHash) && pool.definitionHash != 0
            && tables::read_array(
                blob, at + kPoolEntriesOffset, kEntryClass, kEntryStride, members)) {
            pool.entries.first = static_cast<std::uint32_t>(entries_.size());
            for (std::size_t j = 0; j < members.count; ++j) {
                const auto beforeInstructions = instructions_.size();
                const auto beforeModifiers = modifiers_.size();
                const auto beforeSockets = sockets_.size();
                const char* fullBank = nullptr;
                if (!entry(blob, members.dataOffset + j * kEntryStride, fullBank)) {
                    // Skipping past a full bank would publish a pool missing some of its entries.
                    if (fullBank != nullptr) {
                        settle_unsupported(fullBank);
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

bool RewardBuild::read_item(std::uint32_t hash,
                            std::span<const std::byte> blob,
                            domain::Item& item) noexcept {
    std::uint16_t acquired = domain::kAbsent;
    std::int64_t wrapper = 0;
    std::uint32_t cls = 0;
    if (!tables::read(blob, kWrapperField, wrapper)
        || !tables::read(blob, kAcquiredFlagField, acquired)) {
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
        }
    }
    // A zero delta means the item is not a wrapper.
    if (wrapper != 0) {
        std::size_t at = 0;
        tables::Array selections{};
        if (!tables::relative(blob, kWrapperField, at) || at < kDefinitionClassPrefixSize
            || !tables::read(blob, at - kDefinitionClassPrefixSize, cls) || cls != kWrapperClass
            || !tables::read(blob, at, item.poolIndex)
            || !tables::read(blob, at + kWrapperFlagsOffset, item.flags)
            || !tables::read_array(
                blob, at + kWrapperSelectionsOffset, kSelectionClass, kSelectionStride, selections)
            || selections.count > item.selections.size()) {
            return false;
        }
        item.selectionCount = static_cast<std::uint8_t>(selections.count);
        for (std::size_t j = 0; j < selections.count; ++j) {
            auto& selection = item.selections[j];
            const auto p = selections.dataOffset + j * kSelectionStride;
            if (!tables::read(blob, p, selection.categoryHash)
                || !tables::read(blob, p + kSelectionCountOffset, selection.count)) {
                return false;
            }
        }
    }
    return true;
}

bool RewardBuild::begin_items(std::size_t count) noexcept {
    unboundAcquiredFlags_ = 0;
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

bool RewardBuild::publish() noexcept {
    if (!loaded_) {
        return false;
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
    if (unboundAcquiredFlags_ != 0 || droppedEntries != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=rewards result=partial unbound_acquired_flags=%zu "
                          "dropped_entries=%zu",
                          unboundAcquiredFlags_,
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
