#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../unlocks/unlocks_expression.h"
#include "../items/details/definition.h"
#include "../items/quest_initialization.h"

namespace sunrise::state::build_data::pursuits {

/** A native item reference uses all bits set when it is absent. */
inline constexpr std::uint16_t kAbsent = items::details::kUnavailableItemIndex;
/** The shipped build declares 1,941 pursuit contracts. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 4096;
/** The installed pursuit objective block supports seven displayed objectives. */
inline constexpr std::size_t kObjectiveCapacity = 7;
/** An item's value block holds six fixed reward rows. */
inline constexpr std::size_t kRewardPreviewCapacity = 6;
/**
 * Instructions one reward row's condition expands to. The shipped build declares 318 conditional
 * rows, and the longest expands to seven. A longer one is kept as a row marked unreadable. 8 bytes
 * each, so 1.5 MiB across a full catalog.
 */
inline constexpr std::size_t kRewardConditionCapacity = 8;
/** The item instance record ends in eight per-item values, so a contract maps at most eight. */
inline constexpr std::size_t kItemValueCapacity = 8;

/** One reward row a pursuit shows. */
struct RewardPreview {
    std::uint16_t itemIndex{kAbsent};
    /** Zero when the row names its item without an amount. */
    std::int32_t quantity{};
    /**
     * Shared expression row that decides whether the row applies, or kAbsent for none. Once bound
     * it serves validation only: a row naming an expression holds its program or is unreadable.
     */
    std::uint16_t conditionIndex{kAbsent};
    /** That expression, expanded and bound to the saved banks. */
    std::array<unlocks::Instruction, kRewardConditionCapacity> condition{};
    /** Entries of `condition` in use. Zero when the row names no expression. */
    std::uint8_t conditionCount{};
    /** True when the expression did not read, bind or fit. */
    bool conditionUnreadable{};
};

/** Whether a pursuit belongs to a quest set the server can advance. */
enum class QuestMembership : std::uint8_t {
    /** No set names the item, or the set's only member is the item itself. */
    none,
    /** A mode-1 set whose step value has one saved row. */
    step,
    /** The item names a set whose shape or saved row is not supported. */
    unsupported,
};

/** The item's place in its quest set: its own step value and the member after it. */
struct QuestStep {
    QuestMembership membership{QuestMembership::none};
    /** Saved bank and row holding the set's current step value. */
    items::QuestInitialization::Scope scope{};
    std::uint16_t row{};
    /** Step value naming this item. */
    std::int32_t value{};
    /** The next member and its step value; kAbsent for the last member. */
    std::uint16_t successorIndex{kAbsent};
    std::int32_t successorValue{};
};

/** A value completing the pursuit sets to one, and the saved row it maps to. */
struct CompletionValue {
    /** The contract names the value. */
    bool declared{};
    /** Saved bank and row; scope none when the named value maps to no saved row. */
    items::QuestInitialization::Scope scope{};
    std::uint16_t row{};
};

/** The installed contract of one pursuit item: its objectives and how they combine. */
struct Definition {
    std::uint16_t definitionIndex{kAbsent};
    std::uint32_t definitionHash{};
    bool readable{};
    /** Positive for a bounty, which expires this many seconds after it is acquired. */
    std::int32_t lifetimeSeconds{};
    bool requireAllObjectives{};
    bool requireNonOptionalCompletion{};
    /** Completed objectives never complete the pursuit. */
    bool suppressCompletion{};
    /** The pursuit completes as soon as its objectives do, without a request from the client. */
    bool automaticCompletion{};
    CompletionValue completionValue{};
    CompletionValue completionContextValue{};
    /** Objective-table indices. Slots past objectiveCount stay zero. */
    std::array<std::uint16_t, kObjectiveCapacity> objectives{};
    std::uint8_t objectiveCount{};
    /**
     * Value slots the item's instance record carries, in the order of its per-item values. Slots
     * past itemValueSlotCount stay zero. An item whose list could not be read maps none.
     */
    std::array<std::uint16_t, kItemValueCapacity> itemValueSlots{};
    std::uint8_t itemValueSlotCount{};
    /** False when the value block could not be read, so its rows are unknown rather than empty. */
    bool rewardsReadable{};
    /** Rows past rewardCount keep their defaults. */
    std::array<RewardPreview, kRewardPreviewCapacity> rewards{};
    std::uint8_t rewardCount{};
    QuestStep questStep{};
};

} // namespace sunrise::state::build_data::pursuits
