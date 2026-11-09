#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** Booleans are stored as one byte that must read zero or one. */
[[nodiscard]] bool decode_flag(std::uint8_t stored, bool& value) noexcept {
    value = stored != 0;
    return stored <= 1;
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
    return true;
}

/** Decodes one objective row; the complete-domain validator checks its program ranges. */
bool decode(const ObjectiveRecord& record, objectives::Row& value) noexcept {
    value = {};
    if (!decode_flag(record.optional, value.optional)
        || !decode_flag(record.countsDown, value.countsDown)
        || !decode_flag(record.adjustmentReadable, value.adjustmentReadable)
        || !decode_flag(record.readable, value.readable)) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.completionValue = record.completionValue;
    value.definitionIndex = record.definitionIndex;
    value.targetAdjustment = {record.targetAdjustment.first, record.targetAdjustment.count};
    value.expression = {record.expression.first, record.expression.count};
    value.itemValueSlot = record.itemValueSlot;
    return true;
}

/** Encodes one pursuit contract with its objectives, item values, reward rows and quest step. */
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
        const pursuits::RewardPreview& reward = value.rewards[index];
        record.rewards[index] = {reward.quantity, reward.itemIndex, reward.conditionIndex};
    }
    record.questMembership = static_cast<std::uint8_t>(value.questStep.membership);
    record.questScope = static_cast<std::uint8_t>(value.questStep.scope);
    record.questRow = value.questStep.row;
    record.questValue = value.questStep.value;
    record.questSuccessorIndex = value.questStep.successorIndex;
    record.questSuccessorValue = value.questStep.successorValue;
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
        const PursuitRewardRecord& reward = record.rewards[index];
        value.rewards[index] = {reward.itemIndex, reward.quantity, reward.conditionIndex};
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
