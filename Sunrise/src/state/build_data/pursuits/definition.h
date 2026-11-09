#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
/** The item instance record ends in eight per-item values, so a contract maps at most eight. */
inline constexpr std::size_t kItemValueCapacity = 8;

/** One reward row a pursuit shows. */
struct RewardPreview {
    std::uint16_t itemIndex{kAbsent};
    /** Zero when the row names its item without an amount. */
    std::int32_t quantity{};
    /** Shared expression row that decides whether the row applies, or kAbsent for none. */
    std::uint16_t conditionIndex{kAbsent};
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

/** The installed contract of one pursuit item: its objectives and how they combine. */
struct Definition {
    std::uint16_t definitionIndex{kAbsent};
    std::uint32_t definitionHash{};
    bool readable{};
    /** Positive for a bounty, which expires this many seconds after it is acquired. */
    std::int32_t lifetimeSeconds{};
    bool requireAllObjectives{};
    bool requireNonOptionalCompletion{};
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
