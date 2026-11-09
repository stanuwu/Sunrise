/**
 * Pursuit redemption: a completed pursuit leaves inventory through the shared reward grant, which
 * also moves its quest set to the next step and places what that step pays.
 */
#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "pursuit_completion_runtime.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace inventory_buckets = build_data::inventory::buckets;
namespace pursuits = build_data::pursuits;
using Quest = build_data::items::QuestInitialization;

static_assert(pursuits::kItemValueCapacity == authored_inventory::kItemObjectiveLaneCount);

namespace {

/**
 * What redeeming one pursuit grants. The objectives, per-item values, reward rows and quest set
 * it is built from are read from the installed packages; the effects are reconstructed. Native
 * completion runs operation bodies the packages do not describe, so the server instead removes
 * the pursuit, moves its quest set's saved step to the next member and grants that member. Only
 * the last member, or a pursuit outside any set, grants the reward rows it shows, so a set pays
 * them once.
 */
struct CompletionPlan {
    std::array<DirectRecordReward, kRecordRewardGrantCapacity> rows{};
    std::size_t rowCount{};
    std::bitset<unlocks::kAccountFlagCapacity> wrapperFlags{};
    std::array<reward_resolution::Grant, kRecordRewardGrantCapacity> grants{};
    PreparedQuestStep questStep{};
};

/**
 * A reward row names a stand-in rather than an item when nothing in the packages turns it into
 * inventory: its item is homed in a bucket that keeps only its newest row, or it is a perk-bucket
 * item with no acquisition flag to set. In the shipped build every experience, gear, powerful and
 * pinnacle marker and the Glimmer and Bright Dust stand-ins sit in the build's only one-row FIFO
 * bucket, and the rank-point markers in the perk bucket. What each one pays is server policy the
 * packages do not hold, so the row grants nothing.
 */
[[nodiscard]] bool names_stand_in(std::uint16_t itemIndex) noexcept {
    build_data::items::Definition item{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_item_definition_index(itemIndex, item)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return false;
    }
    if (bucket.slotCount == 1 && (bucket.policyFlags & inventory_buckets::kFifo) != 0) {
        return true;
    }
    build_data::rewards::Item reward{};
    return item.bucketId == inventory_buckets::kPerkBucketId
           && build_data::find_reward_item(itemIndex, reward)
           && reward.acquiredFlag == build_data::rewards::kAbsent;
}

/**
 * A quest step is redeemed here only when every objective it counts is held by the item. A step
 * whose progress lives in the saved banks is advanced by whatever writes those banks.
 */
[[nodiscard]] bool progress_item_held(const PursuitCompletionStatus& status) noexcept {
    const auto objectives = std::span(status.objectives).first(status.objectiveCount);
    return !objectives.empty()
           && std::all_of(objectives.begin(), objectives.end(), [](const auto& objective) {
                  return objective.skipped || objective.itemHeld;
              });
}

/**
 * A bounty's deadline is its first per-item value, so an objective read from that value would
 * count the deadline as progress.
 */
[[nodiscard]] bool reads_deadline(const pursuits::Definition& contract) noexcept {
    if (contract.lifetimeSeconds <= 0 || contract.itemValueSlotCount == 0) {
        return false;
    }
    const std::uint16_t deadline = contract.itemValueSlots[authored_inventory::kItemExpiryLane];
    const auto objectives = std::span(contract.objectives).first(contract.objectiveCount);
    for (const std::uint16_t index : objectives) {
        build_data::objectives::Definition objective{};
        if (build_data::find_objective_definition(index, objective)
            && objective.itemValueSlot == deadline) {
            return true;
        }
    }
    return false;
}

/**
 * Plans the next quest step: the saved step must name this pursuit, and its successor is granted.
 * @return False when the saved step names another member.
 */
[[nodiscard]] bool plan_quest_step(const pursuits::QuestStep& step,
                                   unlocks::Table& banks,
                                   CompletionPlan& plan) noexcept {
    const std::int32_t saved = quest_value(banks, {step.value, step.row, step.scope});
    if (saved != step.value) {
        return false;
    }
    if (step.successorIndex != pursuits::kAbsent) {
        plan.questStep = {{step.successorValue, step.row, step.scope}, saved};
        plan.rows[plan.rowCount++] = {step.successorIndex, 1};
    }
    return true;
}

/**
 * Resolves the reward rows a pursuit shows. A row with no amount or naming a stand-in grants
 * nothing; a wrapper draws from its own pool.
 * @param context Saved banks and class of the redeeming character; its seed starts the draws.
 * @return False when a row is conditional, draws nothing, or outgrows the grant.
 */
[[nodiscard]] bool plan_rewards(const pursuits::Definition& contract,
                                const reward_resolution::Context& context,
                                CompletionPlan& plan,
                                const char*& reason) noexcept {
    const auto rewards = std::span(contract.rewards).first(contract.rewardCount);
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        const pursuits::RewardPreview& reward = rewards[index];
        if (reward.quantity == 0 || names_stand_in(reward.itemIndex)) {
            continue;
        }
        reason = "reward_condition";
        if (reward.conditionIndex != pursuits::kAbsent) {
            return false;
        }
        reward_resolution::Context rowContext = context;
        rowContext.seed = context.seed + index;
        reward_resolution::Result resolved{};
        if (reward_resolution::resolve(rowContext,
                                       reward.itemIndex,
                                       static_cast<std::uint32_t>(reward.quantity),
                                       resolved)
            != reward_resolution::Resolution::resolved) {
            if (reason == nullptr) {
                reason = "reward_draw";
            }
            return false;
        }
        reason = "reward_capacity";
        if (resolved.count > plan.grants.size() - plan.rowCount) {
            return false;
        }
        plan.wrapperFlags |= resolved.wrapperFlags;
        const auto grants = std::span(resolved.grants).first(resolved.count);
        for (const reward_resolution::Grant& grant : grants) {
            plan.grants[plan.rowCount] = grant;
            const reward_resolution::Grant& kept = plan.grants[plan.rowCount];
            plan.rows[plan.rowCount++] = {kept.itemIndex,
                                          kept.quantity,
                                          std::span(kept.sockets).first(kept.socketCount),
                                          true};
        }
    }
    return true;
}

} // namespace

/** Judges completion and plans under one store lock, then prepares the grant from that plan. */
PursuitRedemption prepare_pursuit_redemption(std::uint64_t instanceSoid,
                                             std::uint32_t definitionHash,
                                             std::int64_t nowSeconds,
                                             std::uint64_t seed,
                                             PendingRecordRewardGrant& mutation,
                                             const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "not_ready";
    if (!build_data::pursuit_definitions_ready() || !build_data::reward_definitions_ready()) {
        return PursuitRedemption::notReady;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    const std::unique_ptr<AccountState> account(new (std::nothrow) AccountState{});
    const std::unique_ptr<unlocks::Table> banks(new (std::nothrow) unlocks::Table{});
    reason = "saved_state";
    if (!account || !banks || !investment::store::read_account(*account)) {
        return PursuitRedemption::notReady;
    }
    const std::size_t characterIndex = selected_character_index(*account);
    if (characterIndex >= account->characterCount
        || !investment::store::read_unlocks(*banks, static_cast<int>(characterIndex))) {
        return PursuitRedemption::notReady;
    }
    const CharacterState& character = account->characters[characterIndex];
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    const auto source = std::find_if(held.begin(), held.end(), [instanceSoid](const auto& item) {
        return item.instanceSoid == instanceSoid;
    });
    build_data::items::Definition item{};
    pursuits::Definition contract{};
    reason = "pursuit";
    if (instanceSoid == 0 || source == held.end() || source->definitionHash != definitionHash
        || source->quantity != 1
        || !build_data::find_item_definition_hash(source->definitionHash, item)
        || !build_data::find_pursuit_definition(item.definitionIndex, contract)
        || !contract.readable) {
        return PursuitRedemption::refused;
    }
    reason = "locked";
    if ((source->flags & authored_inventory::kLockedItemFlag) != 0) {
        return PursuitRedemption::refused;
    }
    static constexpr authored_inventory::ItemObjectives kUnsetItemValues{};
    const authored_inventory::ItemObjectives& itemValues =
        source->objectives.has_value() ? *source->objectives : kUnsetItemValues;
    reason = "expired";
    if (contract.lifetimeSeconds > 0) {
        const std::int32_t deadline = itemValues[authored_inventory::kItemExpiryLane];
        if (deadline <= 0 || nowSeconds >= deadline) {
            return PursuitRedemption::notRedeemable;
        }
    }

    const reward_resolution::Context context{*banks, character.characterClass, seed, &reason};
    const PursuitCompletionStatus status =
        inspect_pursuit_completion(contract, context, itemValues);
    const pursuits::QuestStep& step = contract.questStep;
    reason = "quest_progress";
    if (step.membership != pursuits::QuestMembership::none && !progress_item_held(status)) {
        return PursuitRedemption::notRedeemable;
    }
    reason = "objective_source";
    if (!status.objectivesKnown) {
        return PursuitRedemption::refused;
    }
    reason = "objective_reads_deadline";
    if (reads_deadline(contract)) {
        return PursuitRedemption::refused;
    }
    reason = "incomplete";
    if (!status.objectivesComplete) {
        return PursuitRedemption::notRedeemable;
    }

    CompletionPlan plan{};
    reason = "quest_step";
    if (step.membership == pursuits::QuestMembership::unsupported
        || (step.membership == pursuits::QuestMembership::step
            && !plan_quest_step(step, *banks, plan))) {
        return PursuitRedemption::refused;
    }
    reason = "reward_rows";
    const bool paysRewards = step.membership == pursuits::QuestMembership::none
                             || step.successorIndex == pursuits::kAbsent;
    if (paysRewards
        && (!contract.rewardsReadable || !plan_rewards(contract, context, plan, reason))) {
        return PursuitRedemption::refused;
    }

    const RewardPreparation grant =
        prepare_reward_grant(std::span(plan.rows).first(plan.rowCount),
                             kUnclaimedRecordIndex,
                             &plan.wrapperFlags,
                             instanceSoid,
                             mutation,
                             &reason);
    if (grant != RewardPreparation::prepared) {
        return grant == RewardPreparation::notReady ? PursuitRedemption::notReady
                                                    : PursuitRedemption::refused;
    }
    // A step moved on without its item would leave the quest with no step to hold.
    reason = "quest_successor_placement";
    if (plan.questStep.next.scope != Quest::Scope::none
        && mutation.rewards[0].placedQuantity == 0) {
        return PursuitRedemption::refused;
    }
    mutation.questStep = plan.questStep;
    reason = nullptr;
    return PursuitRedemption::prepared;
}

} // namespace sunrise::state
