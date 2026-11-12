#include "package_pursuit_build.h"

#include <algorithm>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/tables/quest_initialization_reader.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"
#include "package_reward_build.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace pursuits = state::build_data::pursuits;

// The reader's row and a contract hold the same lists, and mark an absent reward field alike.
static_assert(tables::items::kObjectiveCapacity == pursuits::kObjectiveCapacity);
static_assert(tables::items::kItemValueCapacity == pursuits::kItemValueCapacity);
static_assert(tables::items::kRewardPreviewCapacity == pursuits::kRewardPreviewCapacity);
static_assert(tables::items::kUnavailableRewardReference == pursuits::kAbsent);

/** @return True when the published item catalog holds the row. */
[[nodiscard]] bool cataloged(std::uint16_t index) noexcept {
    state::build_data::items::Definition item{};
    return state::build_data::find_item_definition_index(index, item);
}

/**
 * Drops what names an item the catalog left out: the contract's reward rows become unknown, and
 * its quest set unsupported.
 * @return True when the contract named such an item.
 */
[[nodiscard]] bool drop_uncataloged_references(pursuits::Definition& row) noexcept {
    const auto rewards = std::span(row.rewards).first(row.rewardCount);
    const bool rewardsMissing =
        std::any_of(rewards.begin(), rewards.end(), [](const pursuits::RewardPreview& reward) {
            return !cataloged(reward.itemIndex);
        });
    const bool successorMissing = row.questStep.successorIndex != pursuits::kAbsent
                                  && !cataloged(row.questStep.successorIndex);
    if (rewardsMissing) {
        row.rewardsReadable = false;
        row.rewards = {};
        row.rewardCount = 0;
    }
    if (successorMissing) {
        row.questStep = {pursuits::QuestMembership::unsupported};
    }
    return rewardsMissing || successorMissing;
}

/** Maps a completion value slot to its saved row. A slot no saved map holds keeps scope none. */
[[nodiscard]] pursuits::CompletionValue
completion_value(std::uint16_t slot, std::span<const std::byte> valueMap) noexcept {
    pursuits::CompletionValue value{};
    value.declared = slot != tables::items::kUnavailableValueSlot;
    if (value.declared && !tables::items::map_value_slot(valueMap, slot, value.row, value.scope)) {
        value.scope = {};
        value.row = 0;
    }
    return value;
}

/**
 * Binds the condition each reward row names. An expression that does not read, bind or fit is kept
 * as a row marked unreadable.
 * @param unreadable Counts the rows kept unreadable.
 * @return False only when the reader's storage failed, which a later pass retries.
 */
[[nodiscard]] bool read_reward_conditions(const RewardConditions& conditions,
                                          pursuits::Definition& output,
                                          std::size_t& unreadable) noexcept {
    for (pursuits::RewardPreview& reward : std::span(output.rewards).first(output.rewardCount)) {
        if (reward.conditionIndex == pursuits::kAbsent) {
            continue;
        }
        std::size_t count = 0;
        BankFailure failure = BankFailure::none;
        const bool readable =
            conditions.read_index(reward.conditionIndex, reward.condition, count, failure);
        if (failure == BankFailure::storage) {
            return false;
        }
        reward.conditionUnreadable = !readable;
        reward.conditionCount = static_cast<std::uint8_t>(count);
        unreadable += static_cast<std::size_t>(!readable);
    }
    return true;
}

} // namespace

/** Logs a refusal the installed data decides and settles the objective and pursuit domains. */
void settle_pursuits_unsupported(const char* reason) noexcept {
    if (state::build_data::pursuit_catalog_unsupported()) {
        return;
    }
    state::build_data::settle_pursuit_catalog_unsupported();
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=pkg stage=pursuits result=unsupported reason=%s",
                      reason);
}

/** @return True when the pursuit contracts are published or can never be. */
bool pursuit_catalog_settled() noexcept {
    return state::build_data::pursuit_definitions_ready()
           || state::build_data::pursuit_catalog_unsupported();
}

/** Starts a new walk, discarding what an earlier one collected. */
void PursuitBuild::begin() noexcept {
    rows_.clear();
    unreadCount_ = 0;
    firstUnread_ = pursuits::kAbsent;
    readFailures_ = 0;
    firstReadFailure_ = pursuits::kAbsent;
    unreadableConditions_ = 0;
}

/** Collects one item's contract, returning false when it could not be stored. */
bool PursuitBuild::item(const tables::items::Row& row,
                        const pursuits::QuestStep& questStep,
                        std::span<const std::byte> valueMap,
                        const RewardConditions& conditions) noexcept {
    // Only pursuits declare completion metadata. An unreadable metadata block still gets a
    // contract, marked unreadable.
    if (!row.hasCompletionMetadata && row.objectiveMetadataValid) {
        return true;
    }
    try {
        pursuits::Definition output{};
        output.definitionIndex = row.definitionIndex;
        output.definitionHash = row.definitionHash;
        output.readable = row.objectiveMetadataValid && row.hasCompletionMetadata;
        output.lifetimeSeconds = row.lifetimeSeconds;
        if (output.readable) {
            output.requireAllObjectives = row.requireAllObjectives;
            output.requireNonOptionalCompletion = row.requireNonOptionalCompletion;
            output.suppressCompletion = row.suppressCompletion;
            output.automaticCompletion = row.automaticCompletion;
            output.completionValue = completion_value(row.completionValueSlot, valueMap);
            output.completionContextValue =
                completion_value(row.completionContextValueSlot, valueMap);
            std::copy_n(row.objectiveIndices, row.objectiveCount, output.objectives.begin());
            output.objectiveCount = row.objectiveCount;
            std::copy_n(
                row.itemValueSlots, row.itemValueSlotCount, output.itemValueSlots.begin());
            output.itemValueSlotCount = row.itemValueSlotCount;
            output.rewardsReadable = row.rewardPreviewsValid;
            for (std::size_t index = 0; index < row.rewardPreviewCount; ++index) {
                const tables::items::RewardPreview& preview = row.rewardPreviews[index];
                output.rewards[index] = {
                    preview.itemIndex, preview.quantity, preview.conditionIndex};
            }
            output.rewardCount = row.rewardPreviewCount;
            if (!read_reward_conditions(conditions, output, unreadableConditions_)) {
                return false;
            }
            output.questStep = questStep;
        }
        rows_.push_back(output);
        return true;
    } catch (...) {
        return false;
    }
}

/** Counts an item the installed data leaves unreadable. That item gets no contract. */
void PursuitBuild::unread(std::uint16_t index) noexcept {
    if (unreadCount_++ == 0) {
        firstUnread_ = index;
    }
}

/** Notes a cataloged item whose read or storage failed, which holds publication back. */
void PursuitBuild::read_failed(std::uint16_t index) noexcept {
    if (readFailures_++ == 0) {
        firstReadFailure_ = index;
    }
}

/** Publishes the collected contracts, or settles the domain when State refuses them. */
void PursuitBuild::publish() noexcept {
    // Every contract is checked against the item and objective tables, so wait for both.
    if (!state::build_data::item_definitions_ready()
        || !state::build_data::objective_definitions_ready()) {
        return;
    }
    // A failed read or allocation left out contracts the data declares. Publishing now would cache
    // the catalog without them, and validating it could settle the domain unsupported over a
    // failure the data did not decide.
    if (readFailures_ != 0) {
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=pkg stage=pursuits result=fail reason=item_read read_failures=%zu "
                          "first_index=%u",
                          readFailures_,
                          static_cast<unsigned>(firstReadFailure_));
        return;
    }
    // A row an earlier pass failed to read stays out of the published item catalog, so its
    // contract is pruned with it rather than settling the domain over an item State never holds.
    const std::size_t pruned = std::erase_if(rows_, [](const pursuits::Definition& row) noexcept {
        state::build_data::items::Definition item{};
        return !state::build_data::find_item_definition_index(row.definitionIndex, item);
    });
    std::size_t unresolved = 0;
    for (pursuits::Definition& row : rows_) {
        unresolved += static_cast<std::size_t>(drop_uncataloged_references(row));
    }
    // The cache write can still fail, and a failed publish repeats the item walk on a later pass.
    const bool published = state::build_data::publish_pursuit_definitions(rows_);
    // Publishing checks the contracts, so only a failed publish is checked again, to tell
    // contracts State refuses from a write that failed.
    const bool valid = published || state::build_data::valid_pursuit_definitions(rows_);
    const std::size_t rowCount = rows_.size();
    // State keeps its own copy, so release the collected rows.
    rows_ = {};
    if (!valid) {
        settle_pursuits_unsupported("contracts");
        return;
    }
    if (!published) {
        return;
    }
    core::log::writef(core::log::Channel::client,
                      unreadCount_ == 0 && pruned == 0 && unresolved == 0
                              && unreadableConditions_ == 0
                          ? core::log::Level::info
                          : core::log::Level::warn,
                      "ev=pkg stage=pursuits result=ready rows=%zu unread=%zu first_unread=%u "
                      "pruned=%zu unresolved=%zu unreadable_conditions=%zu",
                      rowCount,
                      unreadCount_,
                      static_cast<unsigned>(firstUnread_),
                      pruned,
                      unresolved,
                      unreadableConditions_);
}

} // namespace sunrise::client::content::items::packages
