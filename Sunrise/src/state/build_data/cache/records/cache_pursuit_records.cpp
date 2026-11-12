#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** Booleans are stored as one byte that must read zero or one. */
[[nodiscard]] bool decode_flag(std::uint8_t stored, bool& value) noexcept {
    value = stored != 0;
    return stored <= 1;
}

/** Encodes one completion value and the saved row it maps to. */
[[nodiscard]] PursuitCompletionValueRecord
encode_completion_value(const pursuits::CompletionValue& value) noexcept {
    return {static_cast<std::uint8_t>(value.declared),
            static_cast<std::uint8_t>(value.scope),
            value.row};
}

/** Decodes one completion value; the catalog validator checks its saved row. */
[[nodiscard]] bool decode_completion_value(const PursuitCompletionValueRecord& record,
                                           pursuits::CompletionValue& value) noexcept {
    if (!decode_flag(record.declared, value.declared)
        || record.scope > static_cast<std::uint8_t>(items::QuestInitialization::Scope::character)) {
        return false;
    }
    value.scope = static_cast<items::QuestInitialization::Scope>(record.scope);
    value.row = record.row;
    return true;
}

/** Encodes one reward row and the condition bound to it. */
[[nodiscard]] bool encode_reward(const pursuits::RewardPreview& value,
                                 PursuitRewardRecord& record) noexcept {
    record = {};
    record.quantity = value.quantity;
    record.itemIndex = value.itemIndex;
    record.conditionIndex = value.conditionIndex;
    record.conditionCount = value.conditionCount;
    record.conditionUnreadable = static_cast<std::uint8_t>(value.conditionUnreadable);
    if (record.conditionCount > record.condition.size()) {
        return false;
    }
    for (std::size_t index = 0; index < record.condition.size(); ++index) {
        if (!encode(value.condition[index], record.condition[index])) {
            return false;
        }
    }
    return true;
}

/**
 * Decodes one reward row. Only the instructions it counts are decoded, and the slots after them
 * must hold their defaults.
 */
[[nodiscard]] bool decode_reward(const PursuitRewardRecord& record,
                                 pursuits::RewardPreview& value) noexcept {
    value = {};
    if (record.conditionCount > record.condition.size()
        || !decode_flag(record.conditionUnreadable, value.conditionUnreadable)) {
        return false;
    }
    const auto unused = std::span(record.condition).subspan(record.conditionCount);
    if (!std::all_of(unused.begin(), unused.end(), unused_condition)) {
        return false;
    }
    for (std::size_t index = 0; index < record.conditionCount; ++index) {
        if (!decode(record.condition[index], value.condition[index])) {
            return false;
        }
    }
    value.itemIndex = record.itemIndex;
    value.quantity = record.quantity;
    value.conditionIndex = record.conditionIndex;
    value.conditionCount = record.conditionCount;
    return true;
}

} // namespace

/** Encodes one objective row and the bank ranges of its programs. */
bool encode(const objectives::Row& value, ObjectiveRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.completionValue = value.completionValue;
    record.definitionIndex = value.definitionIndex;
    record.optional = static_cast<std::uint8_t>(value.optional);
    record.countsDown = static_cast<std::uint8_t>(value.countsDown);
    record.adjustmentReadable = static_cast<std::uint8_t>(value.adjustmentReadable);
    record.readable = static_cast<std::uint8_t>(value.readable);
    record.targetAdjustment = {value.targetAdjustment.first, value.targetAdjustment.count};
    record.expression = {value.expression.first, value.expression.count};
    record.itemValueSlot = value.itemValueSlot;
    record.itemValueMode = static_cast<std::uint8_t>(value.itemValueMode);
    return true;
}

/** Decodes one objective row; the complete-domain validator checks its program ranges. */
bool decode(const ObjectiveRecord& record, objectives::Row& value) noexcept {
    value = {};
    if (!decode_flag(record.optional, value.optional)
        || !decode_flag(record.countsDown, value.countsDown)
        || !decode_flag(record.adjustmentReadable, value.adjustmentReadable)
        || !decode_flag(record.readable, value.readable)
        || record.itemValueMode > static_cast<std::uint8_t>(objectives::ItemValueMode::maximum)) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.completionValue = record.completionValue;
    value.definitionIndex = record.definitionIndex;
    value.targetAdjustment = {record.targetAdjustment.first, record.targetAdjustment.count};
    value.expression = {record.expression.first, record.expression.count};
    value.itemValueSlot = record.itemValueSlot;
    value.itemValueMode = static_cast<objectives::ItemValueMode>(record.itemValueMode);
    return true;
}

/**
 * Encodes one pursuit contract with its objectives, item values, reward rows, quest step and
 * completion controls.
 */
bool encode(const pursuits::Definition& value, PursuitRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.lifetimeSeconds = value.lifetimeSeconds;
    record.definitionIndex = value.definitionIndex;
    record.readable = static_cast<std::uint8_t>(value.readable);
    record.requireAllObjectives = static_cast<std::uint8_t>(value.requireAllObjectives);
    record.requireNonOptionalCompletion =
        static_cast<std::uint8_t>(value.requireNonOptionalCompletion);
    record.objectiveCount = value.objectiveCount;
    record.objectives = value.objectives;
    record.itemValueSlotCount = value.itemValueSlotCount;
    record.itemValueSlots = value.itemValueSlots;
    record.rewardsReadable = static_cast<std::uint8_t>(value.rewardsReadable);
    record.rewardCount = value.rewardCount;
    for (std::size_t index = 0; index < value.rewards.size(); ++index) {
        if (!encode_reward(value.rewards[index], record.rewards[index])) {
            return false;
        }
    }
    record.questMembership = static_cast<std::uint8_t>(value.questStep.membership);
    record.questScope = static_cast<std::uint8_t>(value.questStep.scope);
    record.questRow = value.questStep.row;
    record.questValue = value.questStep.value;
    record.questSuccessorIndex = value.questStep.successorIndex;
    record.questSuccessorValue = value.questStep.successorValue;
    record.suppressCompletion = static_cast<std::uint8_t>(value.suppressCompletion);
    record.automaticCompletion = static_cast<std::uint8_t>(value.automaticCompletion);
    record.completionValue = encode_completion_value(value.completionValue);
    record.completionContextValue = encode_completion_value(value.completionContextValue);
    return true;
}

/** Decodes one contract; the catalog validator checks its objective and item references. */
bool decode(const PursuitRecord& record, pursuits::Definition& value) noexcept {
    value = {};
    if (record.objectiveCount > record.objectives.size()
        || !decode_flag(record.readable, value.readable)
        || !decode_flag(record.requireAllObjectives, value.requireAllObjectives)
        || !decode_flag(record.requireNonOptionalCompletion, value.requireNonOptionalCompletion)
        || !decode_flag(record.rewardsReadable, value.rewardsReadable)
        || !decode_flag(record.suppressCompletion, value.suppressCompletion)
        || !decode_flag(record.automaticCompletion, value.automaticCompletion)
        || !decode_completion_value(record.completionValue, value.completionValue)
        || !decode_completion_value(record.completionContextValue, value.completionContextValue)
        || record.questMembership
               > static_cast<std::uint8_t>(pursuits::QuestMembership::unsupported)
        || record.questScope
               > static_cast<std::uint8_t>(items::QuestInitialization::Scope::character)) {
        return false;
    }
    const auto unused = std::span(record.objectives).subspan(record.objectiveCount);
    if (!std::all_of(
            unused.begin(), unused.end(), [](std::uint16_t index) { return index == 0; })) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.lifetimeSeconds = record.lifetimeSeconds;
    value.definitionIndex = record.definitionIndex;
    value.objectiveCount = record.objectiveCount;
    value.objectives = record.objectives;
    value.itemValueSlotCount = record.itemValueSlotCount;
    value.itemValueSlots = record.itemValueSlots;
    value.rewardCount = record.rewardCount;
    for (std::size_t index = 0; index < record.rewards.size(); ++index) {
        if (!decode_reward(record.rewards[index], value.rewards[index])) {
            return false;
        }
    }
    value.questStep = {static_cast<pursuits::QuestMembership>(record.questMembership),
                       static_cast<items::QuestInitialization::Scope>(record.questScope),
                       record.questRow,
                       record.questValue,
                       record.questSuccessorIndex,
                       record.questSuccessorValue};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
