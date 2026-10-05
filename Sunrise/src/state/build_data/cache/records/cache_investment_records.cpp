#include <algorithm>

#include "codec.h"

namespace sunrise::state::build_data::cache::records {
namespace {

/** Cache padding fields are always written as zero. */
constexpr unsigned int kReservedFieldValue = 0;

/** A condition slot past a pass row's count holds what a default instruction encodes to. */
bool unused_condition(const RewardInstructionRecord& record) noexcept {
    const rewards::Instruction unused{};
    return record.opcode == static_cast<std::uint8_t>(unused.opcode)
           && record.bank == static_cast<std::uint8_t>(unused.bank)
           && record.reserved == kReservedFieldValue && record.operand == unused.operand;
}

/** A socket slot past a pass row's count holds what a default override encodes to. */
bool unused_socket(const RewardSocketOverrideRecord& record) noexcept {
    const rewards::SocketOverride unused{};
    return record.socketType == unused.socketType && record.plugItem == unused.plugItem
           && record.plugSet == unused.plugSet && record.rollSet == unused.rollSet
           && record.selection == unused.selection;
}

} // namespace

/** Flattens the 12 buckets into 3 parallel arrays with no per-bucket padding. */
bool encode(const abilities::Definition& value, AbilityBucketRecord& record) noexcept {
    record = {};
    record.socketEntryListIndex = value.socketEntryListIndex;
    record.movementEntry = value.selection.movementEntry;
    record.grenadeEntry = value.selection.grenadeEntry;
    record.superEntry = value.selection.superEntry;
    record.meleeEntry = value.selection.meleeEntry;
    record.classEntry = value.selection.classEntry;
    record.overflowCount = value.overflowCount;
    record.overflow = value.overflow;
    for (std::size_t bucket = 0; bucket < abilities::kBucketCapacity; ++bucket) {
        record.bucketKinds[bucket] = value.buckets[bucket].kind;
        record.bucketHashCounts[bucket] = value.buckets[bucket].hashCount;
        for (std::size_t entry = 0; entry < abilities::kBucketHashCapacity; ++entry) {
            record.bucketHashes[bucket * abilities::kBucketHashCapacity + entry] =
                value.buckets[bucket].hashes[entry];
        }
    }
    return true;
}

/** Rebuilds the 12 buckets from the flat disk arrays. */
bool decode(const AbilityBucketRecord& record, abilities::Definition& value) noexcept {
    value = {};
    if (record.overflowCount > abilities::kOverflowCapacity) {
        return false;
    }
    value.socketEntryListIndex = record.socketEntryListIndex;
    value.selection.movementEntry = record.movementEntry;
    value.selection.grenadeEntry = record.grenadeEntry;
    value.selection.superEntry = record.superEntry;
    value.selection.meleeEntry = record.meleeEntry;
    value.selection.classEntry = record.classEntry;
    value.overflowCount = record.overflowCount;
    value.overflow = record.overflow;
    for (std::size_t bucket = 0; bucket < abilities::kBucketCapacity; ++bucket) {
        if (record.bucketHashCounts[bucket] > abilities::kBucketHashCapacity) {
            return false;
        }
        value.buckets[bucket].kind = record.bucketKinds[bucket];
        value.buckets[bucket].hashCount = record.bucketHashCounts[bucket];
        for (std::size_t entry = 0; entry < abilities::kBucketHashCapacity; ++entry) {
            value.buckets[bucket].hashes[entry] =
                record.bucketHashes[bucket * abilities::kBucketHashCapacity + entry];
        }
    }
    return true;
}

/** Encodes one progression definition with its padding zeroed. */
bool encode(const progressions::Definition& value, ProgressionRecord& record) noexcept {
    record = {
        value.definitionIndex,
        value.stepOffset,
        value.stepCount,
        static_cast<std::uint8_t>(value.scope),
    };
    return true;
}

/** Decodes one progression definition; the complete-domain validator checks its step range. */
bool decode(const ProgressionRecord& record, progressions::Definition& value) noexcept {
    value = {record.definitionIndex,
             record.stepOffset,
             record.stepCount,
             static_cast<progressions::Scope>(record.scope)};
    return true;
}

/** Encodes one progression rank step. */
bool encode(const progressions::Step& value, ProgressionStepRecord& record) noexcept {
    record = {value.cost};
    return true;
}

/** Decodes one progression rank step. */
bool decode(const ProgressionStepRecord& record, progressions::Step& value) noexcept {
    value = {record.cost};
    return true;
}

/** Encodes one season pass reward row with its padding zeroed. */
bool encode(const season_pass::Reward& value, SeasonPassRewardRecord& record) noexcept {
    record = {};
    record.itemHash = value.itemHash;
    record.quantity = value.quantity;
    record.itemIndex = value.itemIndex;
    record.claimFlagIndex = value.claimFlagIndex;
    record.requiredRank = value.requiredRank;
    record.socketCount = value.socketCount;
    record.conditionCount = value.conditionCount;
    if (record.conditionCount > record.condition.size()
        || record.socketCount > record.sockets.size()) {
        return false;
    }
    for (std::size_t i = 0; i < record.condition.size(); ++i) {
        if (!encode(value.condition[i], record.condition[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < record.sockets.size(); ++i) {
        if (!encode(value.sockets[i], record.sockets[i])) {
            return false;
        }
    }
    return true;
}

/** Decodes one season pass reward row after checking its padding and unused slots. */
bool decode(const SeasonPassRewardRecord& record, season_pass::Reward& value) noexcept {
    value = {};
    if (record.reserved != decltype(record.reserved){}
        || record.conditionCount > record.condition.size()
        || record.socketCount > record.sockets.size()) {
        return false;
    }
    const auto unusedConditions = std::span(record.condition).subspan(record.conditionCount);
    const auto unusedSockets = std::span(record.sockets).subspan(record.socketCount);
    if (!std::all_of(unusedConditions.begin(), unusedConditions.end(), unused_condition)
        || !std::all_of(unusedSockets.begin(), unusedSockets.end(), unused_socket)) {
        return false;
    }
    value.itemHash = record.itemHash;
    value.quantity = record.quantity;
    value.itemIndex = record.itemIndex;
    value.claimFlagIndex = record.claimFlagIndex;
    value.requiredRank = record.requiredRank;
    value.socketCount = record.socketCount;
    value.conditionCount = record.conditionCount;
    for (std::size_t i = 0; i < value.conditionCount; ++i) {
        if (!decode(record.condition[i], value.condition[i])) {
            return false;
        }
    }
    for (std::size_t i = 0; i < value.socketCount; ++i) {
        if (!decode(record.sockets[i], value.sockets[i])) {
            return false;
        }
    }
    return true;
}

/** Encodes one repeatable bounty row with its padding zeroed. */
bool encode(const bounties::Definition& value, BountyRecord& record) noexcept {
    record = {value.itemType.bank, value.itemType.hash, value.itemIndex, kReservedFieldValue};
    return true;
}

/** Decodes one repeatable bounty row after checking its padding. */
bool decode(const BountyRecord& record, bounties::Definition& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue) {
        return false;
    }
    value = {{record.itemTypeBank, record.itemTypeHash}, record.itemIndex};
    return true;
}

/** Encodes one record objective row with its padding zeroed. */
bool encode(const build_data::records::Objective& value, RecordObjectiveRecord& record) noexcept {
    record = {value.completionValue,
              value.valueIndex,
              value.sourceValueIndex,
              value.sourceValueSlot,
              kReservedFieldValue};
    return true;
}

/** Decodes one record objective row after checking its padding. */
bool decode(const RecordObjectiveRecord& record, build_data::records::Objective& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue) {
        return false;
    }
    value.completionValue = record.completionValue;
    value.valueIndex = record.valueIndex;
    value.sourceValueIndex = record.sourceValueIndex;
    value.sourceValueSlot = record.sourceValueSlot;
    return true;
}

/** Encodes one record interval step with its padding zeroed. */
bool encode(const build_data::records::Interval& value, RecordIntervalRecord& record) noexcept {
    record = {value.completionValue, value.score, value.itemIndex, kReservedFieldValue};
    return true;
}

/** Decodes one record interval step after checking its padding. */
bool decode(const RecordIntervalRecord& record, build_data::records::Interval& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue) {
        return false;
    }
    value.completionValue = record.completionValue;
    value.score = record.score;
    value.itemIndex = record.itemIndex;
    return true;
}

/** Encodes one record reward row with its padding zeroed. */
bool encode(const build_data::records::Reward& value, RecordRewardRecord& record) noexcept {
    record = {value.quantity, value.itemIndex, kReservedFieldValue};
    return true;
}

/** Decodes one record reward row after checking its padding. */
bool decode(const RecordRewardRecord& record, build_data::records::Reward& value) noexcept {
    value = {};
    if (record.reserved != kReservedFieldValue) {
        return false;
    }
    value.quantity = record.quantity;
    value.itemIndex = record.itemIndex;
    return true;
}

/** Encodes one record with its padding zeroed. */
bool encode(const build_data::records::Definition& value, RecordDefinitionRecord& record) noexcept {
    record = {
        value.definitionIndex,
        value.definitionHash,
        value.completionFlagIndex,
        value.loreRow,
        value.scoreValue,
        value.categoryValueIndex,
        value.objectiveValueIndex,
        value.objectiveOffset,
        value.intervalOffset,
        value.rewardOffset,
        value.redeemedCountValueIndex,
        value.objectiveCount,
        value.intervalCount,
        value.rewardCount,
        static_cast<std::uint8_t>(value.hasTitle),
        0,
    };
    return true;
}

/** Decodes one record after checking its padding. */
bool decode(const RecordDefinitionRecord& record, build_data::records::Definition& value) noexcept {
    value = {};
    // Assign by name. A positional list quietly misfills every field added after this row.
    value.definitionIndex = record.definitionIndex;
    value.definitionHash = record.definitionHash;
    value.completionFlagIndex = record.completionFlagIndex;
    value.loreRow = record.loreRow;
    value.scoreValue = record.scoreValue;
    value.categoryValueIndex = record.categoryValueIndex;
    value.objectiveValueIndex = record.objectiveValueIndex;
    value.objectiveOffset = record.objectiveOffset;
    value.intervalOffset = record.intervalOffset;
    value.rewardOffset = record.rewardOffset;
    value.redeemedCountValueIndex = record.redeemedCountValueIndex;
    value.objectiveCount = record.objectiveCount;
    value.intervalCount = record.intervalCount;
    value.rewardCount = record.rewardCount;
    if (record.hasTitle > 1 || record.reserved != 0) {
        return false;
    }
    value.hasTitle = record.hasTitle != 0;
    return true;
}

/** Encodes one presentation node with canonical unused child rows. */
bool encode(const nodes::Definition& value, NodeDefinitionRecord& record) noexcept {
    if (value.childCount > nodes::kChildCapacity) {
        return false;
    }
    record = {};
    record.definitionIndex = value.definitionIndex;
    record.valueIndex = value.valueIndex;
    record.valueSlot = value.valueSlot;
    record.characterValueSlot = value.characterValueSlot;
    record.parentValueIndex = value.parentValueIndex;
    record.parentCharacterValueIndex = value.parentCharacterValueIndex;
    record.visibilityFlagIndex = value.visibilityFlagIndex;
    record.visibilityCharacterFlagIndex = value.visibilityCharacterFlagIndex;
    record.characterValueIndex = value.characterValueIndex;
    record.loreBook = static_cast<std::uint8_t>(value.loreBook);
    record.childCount = value.childCount;
    for (std::size_t index = 0; index < value.childCount; ++index) {
        record.children[index] = value.children[index];
    }
    return true;
}

/** Decodes one presentation node after checking its child list. */
bool decode(const NodeDefinitionRecord& record, nodes::Definition& value) noexcept {
    value = {};
    if (record.childCount > nodes::kChildCapacity || record.loreBook > 1) {
        return false;
    }
    for (std::size_t index = record.childCount; index < record.children.size(); ++index) {
        if (record.children[index] != 0) {
            return false;
        }
    }
    value.definitionIndex = record.definitionIndex;
    value.valueIndex = record.valueIndex;
    value.valueSlot = record.valueSlot;
    value.characterValueSlot = record.characterValueSlot;
    value.parentValueIndex = record.parentValueIndex;
    value.parentCharacterValueIndex = record.parentCharacterValueIndex;
    value.visibilityFlagIndex = record.visibilityFlagIndex;
    value.visibilityCharacterFlagIndex = record.visibilityCharacterFlagIndex;
    value.characterValueIndex = record.characterValueIndex;
    value.loreBook = record.loreBook != 0;
    value.childCount = record.childCount;
    value.children = record.children;
    return true;
}

/** Encodes one incident-target definition. */
bool encode(const sobjects::Definition& value, SObjectDefinitionRecord& record) noexcept {
    record = {value.nameHash, value.lane4, value.typeCode};
    return true;
}

/** Decodes one incident-target definition. */
bool decode(const SObjectDefinitionRecord& record, sobjects::Definition& value) noexcept {
    value = {record.nameHash, record.lane4, record.typeCode};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
