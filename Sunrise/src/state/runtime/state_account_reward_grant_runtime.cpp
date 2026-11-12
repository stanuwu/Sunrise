/**
 * Record-reward grants: item rows, acquisition flags and quest starts committed as one transaction.
 * Triumph claims, Season pass claims and wrapper draws all prepare through here.
 */
#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../unlocks/unlocks_records.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;
namespace family4_loadout = middleware::datagen::family4::loadout;
using Quest = build_data::items::QuestInitialization;

namespace {

/**
 * @return The kind of item a reward grants; overflow can still land a stack in its recovery
 *         bucket.
 */
[[nodiscard]] RecordRewardKind reward_kind(const item_details::Definition& detail,
                                           const inventory_buckets::Descriptor& bucket) noexcept {
    if (detail.instancedDefinitionState == item_details::InstancedDefinitionState::instanced) {
        return RecordRewardKind::characterInstance;
    }
    return bucket.arraySelector == inventory_buckets::ArraySelector::profile
               ? RecordRewardKind::profileStack
               : RecordRewardKind::characterStack;
}

/** @return True when the character holds the instance unequipped. */
[[nodiscard]] bool holds_unequipped(const CharacterState& character, std::uint64_t soid) noexcept {
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    return std::any_of(
        held.begin(), held.end(), [soid](const auto& item) { return item.instanceSoid == soid; });
}

/**
 * Removes a redeemed pursuit's row. The rows after it keep their serials, which also order the
 * client's bucket cells.
 * @return False when the character does not hold the instance unequipped.
 */
[[nodiscard]] bool remove_redeemed(CharacterState& character, std::uint64_t soid) noexcept {
    auto& inventory = character.inventory;
    const auto held = std::span(inventory.values).first(inventory.count);
    const auto found = std::find_if(
        held.begin(), held.end(), [soid](const auto& item) { return item.instanceSoid == soid; });
    if (found == held.end()) {
        return false;
    }
    std::move(found + 1, held.end(), found);
    inventory.values[--inventory.count] = {};
    return true;
}

/**
 * Checks one prepared reward against its own item rules. The rows it placed are checked against
 * the after-image separately, since eviction later in the batch can move or remove them.
 * @param reward Reward to check.
 * @return False when the reward breaks a rule its kind carries.
 */
[[nodiscard]] bool reward_row_valid(const PreparedRecordReward& reward) noexcept {
    build_data::items::Definition item{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (reward.definitionHash == authored_inventory::kNoDefinitionHash || reward.quantity <= 0
        || reward.placedQuantity < 0 || reward.placedQuantity > reward.quantity
        || !build_data::find_item_definition_hash(reward.definitionHash, item)
        || !build_data::items::valid(reward.quest)) {
        return false;
    }
    // Only a placed character instance starts a quest, and only the quest its definition names.
    const bool startsQuest =
        reward.kind == RecordRewardKind::characterInstance && reward.placedQuantity != 0;
    if (startsQuest ? reward.quest != item.questInitialization
                    : reward.quest.scope != Quest::Scope::none) {
        return false;
    }
    // A placed ingredient pickup always carries its balance credit, and nothing else carries one.
    if (reward.ingredientCredit.has_value()
        != (reward.placedQuantity != 0 && is_ingredient_pickup(reward.definitionHash))) {
        return false;
    }
    // Only a placed oven carries chooser readiness.
    if (reward.chooserReadiness.has_value()
        && (reward.placedQuantity == 0 || !is_dawning_oven(reward.definitionHash))) {
        return false;
    }
    if (reward.kind == RecordRewardKind::accountUnlock) {
        build_data::rewards::Item source{};
        return reward.quantity == 1 && reward.placedQuantity == 1 && reward.instanceSoid == 0
               && item.bucketId == build_data::inventory::buckets::kPerkBucketId
               && build_data::find_reward_item(item.definitionIndex, source)
               && source.acquiredFlag != build_data::rewards::kAbsent
               && source.acquiredFlag == reward.acquiredFlag;
    }
    if (!build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex
        || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
        || reward.kind != reward_kind(detail, bucket)) {
        return false;
    }
    // A placed instance names the identity it took; a stack carries none.
    return reward.kind == RecordRewardKind::characterInstance
               ? reward.quantity == 1 && (reward.placedQuantity != 0) == (reward.instanceSoid != 0)
               : reward.instanceSoid == 0;
}

/** @return True when the grant moves a saved value, credits a progression or pays seasonal XP. */
[[nodiscard]] bool carries_saved_effects(const PendingRecordRewardGrant& mutation) noexcept {
    const auto moves = mutation.saved_value_moves();
    return std::any_of(moves.begin(),
                       moves.end(),
                       [](const PreparedQuestStep* move) noexcept {
                           return move->next.scope != Quest::Scope::none;
                       })
           || mutation.progression.scope != build_data::progressions::Scope::unreplicated
           || mutation.seasonalExperience != 0;
}

/**
 * Rebuilds the account after-image for one prepared reward set and rejects any drift.
 * @param account Holds the live account the mutation was prepared against, and receives the
 *        after-image in place; its content is unusable on failure.
 * @return False when the account moved or a reward row breaks its own item rules.
 */
[[nodiscard]] bool materialize_record_reward(const PendingRecordRewardGrant& mutation,
                                             AccountState& account) noexcept {
    if (!mutation.prepared
        || (mutation.rewardCount == 0 && mutation.wrapperFlagCount == 0 && mutation.rowCount == 0
            && mutation.redeemedInstanceSoid == 0 && !carries_saved_effects(mutation))
        || mutation.rewardCount > mutation.rewards.size()
        || mutation.rowCount > mutation.rows.size()
        || !change_rings_fit(std::span(mutation.rows).first(mutation.rowCount))
        || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.characterIndex >= account.characterCount
        || account.primarySoid != mutation.accountSoid
        || !same_character(account.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            account, mutation.beforeProfileItems, mutation.beforeProfileItemCount)
        || !mutation.beforeCharacter.selected
        || mutation.beforeCharacter.soid != mutation.characterSoid
        || (mutation.redeemedInstanceSoid != 0
            && (!holds_unequipped(mutation.beforeCharacter, mutation.redeemedInstanceSoid)
                || holds_unequipped(mutation.afterCharacter, mutation.redeemedInstanceSoid)))) {
        return false;
    }

    account.characters[mutation.characterIndex] = mutation.afterCharacter;
    account.profileItems = mutation.afterProfileItems;
    account.profileItemCount = mutation.afterProfileItemCount;
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(account) || !valid_profile_inventory(account)
        || !family4_loadout::resolve(account, mutation.characterIndex, loadout)) {
        return false;
    }
    for (std::size_t index = 0; index < mutation.rewardCount; ++index) {
        if (!reward_row_valid(mutation.rewards[index])) {
            return false;
        }
    }
    // Every announced row is in the after-image exactly once.
    for (std::size_t index = 0; index < mutation.rowCount; ++index) {
        const GrantedRow& row = mutation.rows[index];
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mutation.rows[prior].array == row.array
                && mutation.rows[prior].mutationSerial == row.mutationSerial) {
                return false;
            }
        }
        if (!row_present(account, mutation.characterIndex, row)) {
            return false;
        }
    }
    return true;
}

/**
 * Captures the flags of consumed wrappers in ascending order with the values they hold now.
 * @param flags Flags a draw consumed, or null for direct rows.
 * @param reason Receives a static reason on failure.
 * @return Unresolvable when the draw consumed more flagged wrappers than a grant carries;
 *         not ready when a saved flag cannot be read.
 */
[[nodiscard]] RewardPreparation
prepare_wrapper_flags(const std::bitset<unlocks::kAccountFlagCapacity>* flags,
                      PendingRecordRewardGrant& mutation,
                      const char*& reason) noexcept {
    if (flags == nullptr) {
        return RewardPreparation::prepared;
    }
    reason = "wrapper_flag_capacity";
    if (flags->count() > mutation.wrapperFlags.size()) {
        return RewardPreparation::unresolvable;
    }
    reason = "wrapper_acquisition_flag";
    for (std::size_t index = 0; index < flags->size(); ++index) {
        if (!(*flags)[index]) {
            continue;
        }
        std::int32_t before = 0;
        if (!investment::store::read_unlock(
                investment::store::Bank::accountFlags, static_cast<std::uint16_t>(index), before)
            || before < 0 || before > unlocks::kFlagSet) {
            return RewardPreparation::notReady;
        }
        mutation.wrapperFlags[mutation.wrapperFlagCount++] = {static_cast<std::uint16_t>(index),
                                                              static_cast<std::uint8_t>(before)};
    }
    return RewardPreparation::prepared;
}

/**
 * Captures the acquisition flag a resolved reward row sets and the value it holds now.
 * @return Unresolvable when the row has no reward entry; not ready when the flag cannot be read.
 */
[[nodiscard]] RewardPreparation acquisition_flag(const build_data::items::Definition& item,
                                                 PreparedRecordReward& prepared) noexcept {
    build_data::rewards::Item source{};
    if (!build_data::find_reward_item(item.definitionIndex, source)
        || source.definitionHash != item.definitionHash) {
        return RewardPreparation::unresolvable;
    }
    prepared.acquiredFlag = source.acquiredFlag;
    if (source.acquiredFlag == build_data::rewards::kAbsent) {
        return RewardPreparation::prepared;
    }
    std::int32_t before = 0;
    // An unreadable saved flag decides nothing about the definition.
    if (!investment::store::read_unlock(
            investment::store::Bank::accountFlags, source.acquiredFlag, before)
        || before < 0 || before > unlocks::kFlagSet) {
        return RewardPreparation::notReady;
    }
    prepared.previousFlag = static_cast<std::uint8_t>(before);
    return RewardPreparation::prepared;
}

/** The selected character's working view, shared by every row of one grant. */
struct GrantView {
    AccountState& working;
    std::size_t characterIndex{};
    std::span<const PreparedRecordReward> prior;
    /** Lowest identity a new instance may take; placement advances it past each one used. */
    std::uint64_t& nextInstanceSoid;
    PlacedRows& placed;
    /** Server time in Unix seconds; a granted bounty's lifetime runs from it. */
    std::int64_t nowSeconds{};
};

/**
 * Reads the saved step a placed quest item starts from. Two rows that start one quest would each
 * see it unset, so the second start is refused.
 * @param reason Receives a static reason on failure.
 * @return Unresolvable for a duplicate or malformed quest; not ready when the value cannot be read.
 */
[[nodiscard]] RewardPreparation prepare_quest_start(GrantView view,
                                                    const build_data::items::Definition& item,
                                                    PreparedRecordReward& prepared,
                                                    const char*& reason) noexcept {
    const Quest& quest = item.questInitialization;
    reason = "duplicate_quest";
    for (const auto& earlier : view.prior) {
        if (earlier.quest.scope == quest.scope && earlier.quest.row == quest.row) {
            return RewardPreparation::unresolvable;
        }
    }
    reason = "quest_state";
    if (!build_data::items::valid(quest)) {
        return RewardPreparation::unresolvable;
    }
    if (!investment::store::read_unlock(
            quest_bank(quest), quest.row, prepared.previousQuestValue)) {
        return RewardPreparation::notReady;
    }
    prepared.quest = quest;
    return RewardPreparation::prepared;
}

/**
 * Stages the oven balance a placed ingredient pickup credits, by the quantity that found a row. A
 * later pickup of the same balance in the batch credits on top of the earlier one.
 * @param reason Receives a static reason on failure.
 * @return Unresolvable when the pickup's balance row does not resolve; not ready when the balance
 *         cannot be read; deferred when it cannot hold the credit.
 */
[[nodiscard]] RewardPreparation prepare_pickup_credit(GrantView view,
                                                      PreparedRecordReward& prepared,
                                                      const char*& reason) noexcept {
    reason = "ingredient_credit";
    AccountUnlockWrite credit{};
    const RewardPreparation read =
        prepare_ingredient_credit(prepared.definitionHash, prepared.placedQuantity, credit);
    if (read != RewardPreparation::prepared) {
        return read;
    }
    for (const auto& earlier : view.prior) {
        if (earlier.ingredientCredit && earlier.ingredientCredit->index == credit.index) {
            credit.previousValue = earlier.ingredientCredit->value;
        }
    }
    if (credit.previousValue
        > (std::numeric_limits<std::int32_t>::max)() - prepared.placedQuantity) {
        return RewardPreparation::deferred;
    }
    credit.value = credit.previousValue + prepared.placedQuantity;
    prepared.ingredientCredit = credit;
    return RewardPreparation::prepared;
}

/**
 * Stages the chooser readiness a placed oven writes. An earlier oven in the batch already wrote
 * it, so a later one writes nothing.
 * @param reason Receives a static reason on failure.
 * @return Unresolvable when the published contracts name no tutorial quest row; not ready while
 *         they are unpublished or the row cannot be read.
 */
[[nodiscard]] RewardPreparation prepare_oven_readiness(GrantView view,
                                                       PreparedRecordReward& prepared,
                                                       const char*& reason) noexcept {
    reason = "chooser_readiness";
    const bool staged = std::any_of(
        view.prior.begin(), view.prior.end(), [](const PreparedRecordReward& earlier) noexcept {
            return earlier.chooserReadiness.has_value();
        });
    if (staged) {
        return RewardPreparation::prepared;
    }
    return prepare_chooser_readiness(view.working, prepared.chooserReadiness);
}

/**
 * @return True for a perk-bucket item the reward graph gives an acquisition flag. One it gives
 *         none, such as an ingredient pickup, is an inventory row in that FIFO bucket instead.
 */
[[nodiscard]] bool flagged_perk(const build_data::items::Definition& item) noexcept {
    build_data::rewards::Item source{};
    const bool flagless = build_data::find_reward_item(item.definitionIndex, source)
                          && source.acquiredFlag == build_data::rewards::kAbsent;
    return item.bucketId == build_data::inventory::buckets::kPerkBucketId && !flagless;
}

/**
 * Reads where a row's item lands and checks that the destination can hold what the row asks.
 * @param detail Receives the item's configured detail.
 * @param kind Receives the kind of row the item becomes.
 * @param reason Receives a static reason on failure.
 * @return False when the item's own data gives the row no destination, which is final.
 */
[[nodiscard]] bool resolve_destination(const DirectRecordReward& requested,
                                       const build_data::items::Definition& item,
                                       item_details::Definition& detail,
                                       RecordRewardKind& kind,
                                       const char*& reason) noexcept {
    reason = "item_detail";
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_configured_item_detail(requested.itemDefinitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex
        || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return false;
    }
    kind = reward_kind(detail, bucket);
    const bool character = bucket.arraySelector == inventory_buckets::ArraySelector::character;
    const bool instance = kind == RecordRewardKind::characterInstance;
    reason = "socket_owner";
    if (!requested.sockets.empty() && !instance) {
        return false;
    }
    // Only a character instance can start a quest, so any other destination refuses one.
    reason = "quest_destination";
    if (item.questInitialization.scope != Quest::Scope::none && !instance) {
        return false;
    }
    reason = "instance_quantity";
    if (instance && requested.quantity != 1) {
        return false;
    }
    reason = "inventory_destination";
    return (character || bucket.arraySelector == inventory_buckets::ArraySelector::profile)
           && (!instance || character) && (instance || !detail.equipmentSlot.has_value());
}

/**
 * Authors the instance an arrival places: the character's level, the row's socket overrides and a
 * bounty's deadline.
 * @param reason Receives a static reason on failure.
 * @return False when the sockets or the deadline cannot be authored, which is final.
 */
[[nodiscard]] bool author_instance(GrantView view,
                                   std::span<const build_data::rewards::SocketOverride> sockets,
                                   Arrival& arrival,
                                   const char*& reason) noexcept {
    authored_inventory::Item& instance = arrival.instance;
    instance.level = acquisition_level(view.working.characters[view.characterIndex]);
    instance.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
    if (!apply_reward_sockets(arrival.detail, sockets, instance.sockets, &reason)) {
        return false;
    }
    reason = "bounty_deadline";
    return acquired_objectives(arrival.item.definitionIndex, view.nowSeconds, instance.objectives);
}

/**
 * Stages what acquiring a placed item changes beside its row: a new instance's identity, an
 * ingredient pickup's balance credit, an oven's chooser readiness and a quest item's first step.
 * @param reason Receives a static reason when the result is not prepared.
 */
[[nodiscard]] RewardPreparation
prepare_acquisition_effects(GrantView view,
                            const build_data::items::Definition& item,
                            PreparedRecordReward& prepared,
                            const char*& reason) noexcept {
    if (prepared.kind == RecordRewardKind::characterInstance) {
        prepared.instanceSoid = view.nextInstanceSoid - 1;
    }
    if (is_ingredient_pickup(item.definitionHash)) {
        return prepare_pickup_credit(view, prepared, reason);
    }
    if (is_dawning_oven(item.definitionHash)) {
        const auto readiness = prepare_oven_readiness(view, prepared, reason);
        if (readiness != RewardPreparation::prepared) {
            return readiness;
        }
    }
    return item.questInitialization.scope != Quest::Scope::none
               ? prepare_quest_start(view, item, prepared, reason)
               : RewardPreparation::prepared;
}

/**
 * Prepares one requested row against the working view and the rows before it. The row lands the
 * way the client places it, so a full bucket overflows or drops the remainder rather than refusing.
 * @param reason Receives a static reason when the row is not prepared.
 * @return Deferred only when State may still make the row fit; not ready when a saved read
 *         failed; unresolvable is final.
 */
[[nodiscard]] RewardPreparation prepare_reward_row(GrantView view,
                                                   const DirectRecordReward& requested,
                                                   PreparedRecordReward& prepared,
                                                   const char*& reason) noexcept {
    reason = "item_identity";
    build_data::items::Definition item{};
    if (requested.quantity <= 0
        || !build_data::find_item_definition_index(requested.itemDefinitionIndex, item)) {
        return RewardPreparation::unresolvable;
    }
    prepared.definitionHash = item.definitionHash;
    prepared.quantity = requested.quantity;
    if (requested.acquireUnlock) {
        reason = "acquisition_flag";
        // Only a resolved reward asks for its flag, so the graph is published.
        const auto flag = acquisition_flag(item, prepared);
        if (flag != RewardPreparation::prepared) {
            return flag;
        }
    }
    if (flagged_perk(item)) {
        reason = "perk_acquisition";
        if (prepared.acquiredFlag == build_data::rewards::kAbsent || requested.quantity != 1
            || !requested.sockets.empty() || item.questInitialization.scope != Quest::Scope::none) {
            return RewardPreparation::unresolvable;
        }
        prepared.kind = RecordRewardKind::accountUnlock;
        prepared.placedQuantity = 1;
        return RewardPreparation::prepared;
    }
    item_details::Definition detail{};
    if (!resolve_destination(requested, item, detail, prepared.kind, reason)) {
        return RewardPreparation::unresolvable;
    }
    Arrival arrival{item, detail, requested.quantity};
    if (prepared.kind == RecordRewardKind::characterInstance
        && !author_instance(view, requested.sockets, arrival, reason)) {
        return RewardPreparation::unresolvable;
    }
    const auto placement = place_arrival(view.working,
                                         view.characterIndex,
                                         arrival,
                                         view.nextInstanceSoid,
                                         view.placed,
                                         prepared.placedQuantity,
                                         reason);
    if (placement != RewardPreparation::prepared) {
        return placement;
    }
    // Only an item that found a row runs its acquisition side effects.
    return prepared.placedQuantity != 0 ? prepare_acquisition_effects(view, item, prepared, reason)
                                        : RewardPreparation::prepared;
}

/**
 * Removes the held items a grant redeems and replaces, so their rows are free for the grant's own.
 * @param reason Receives a static reason on failure.
 * @return False when the character does not hold one of them unequipped.
 */
[[nodiscard]] bool remove_spent_instances(const RewardGrantRequest& request,
                                          CharacterState& character,
                                          PendingRecordRewardGrant& mutation,
                                          const char*& reason) noexcept {
    // The redeemed item leaves before the rows are placed, so its row is free for them.
    reason = "redeemed_instance";
    if (request.redeemedInstanceSoid != 0
        && !remove_redeemed(character, request.redeemedInstanceSoid)) {
        return false;
    }
    mutation.redeemedInstanceSoid = request.redeemedInstanceSoid;
    // A replaced item leaves the same way; its release publishes with the new rows.
    reason = "replaced_instance";
    return request.replacedInstanceSoid == 0
           || remove_redeemed(character, request.replacedInstanceSoid);
}

/**
 * Spends a vendor row's price in the after-image its rows land in, and advances the Dawning
 * bounties that count a cookie it spends.
 * @param price The row's cost entries; empty spends nothing.
 * @param reason Receives a static reason when the result is not prepared.
 * @return Deferred when the account does not hold the price; not ready when an allocation failed
 *         or a catalog was cleared meanwhile; otherwise as credit_dawning_bounties ended.
 */
[[nodiscard]] RewardPreparation spend_price(std::span<const build_data::vendors::SaleCost> price,
                                            std::int64_t nowSeconds,
                                            AccountState& working,
                                            std::size_t characterIndex,
                                            const char*& reason) noexcept {
    reason = "sale_price";
    if (!price.empty()) {
        const std::unique_ptr<AccountState> charged(new (std::nothrow) AccountState{});
        bool changed = false;
        if (!charged) {
            return RewardPreparation::notReady;
        }
        if (!apply_sale_price(working, price, *charged, changed)) {
            return RewardPreparation::deferred;
        }
        working = *charged;
    }
    // A delivery spends a cookie, which advances the Dawning bounties that count it.
    reason = "dawning_bounty";
    for (const auto& cost : price) {
        build_data::items::Definition spent{};
        if (!build_data::find_item_definition_index(cost.itemIndex, spent)) {
            // The charge found this item, so only a settled catalog rules it out for good.
            return catalogs_settled() ? RewardPreparation::unresolvable
                                      : RewardPreparation::notReady;
        }
        const RewardPreparation credited =
            credit_dawning_bounties(working.characters[characterIndex],
                                    DawningEvent::delivery,
                                    spent.definitionHash,
                                    nowSeconds);
        if (credited != RewardPreparation::prepared) {
            return credited;
        }
    }
    return RewardPreparation::prepared;
}

/**
 * Prepares every requested row in order, each against the working view the rows before it left.
 * @param view The grant's working view; its prior rows are set for each row in turn.
 * @param reason Receives a static reason when a row is not prepared.
 * @return How the first row that was not prepared ended, else prepared.
 */
[[nodiscard]] RewardPreparation prepare_reward_rows(GrantView view,
                                                    std::span<const DirectRecordReward> rewards,
                                                    PendingRecordRewardGrant& mutation,
                                                    const char*& reason) noexcept {
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        view.prior = std::span(mutation.rewards).first(index);
        PreparedRecordReward prepared{};
        const auto row = prepare_reward_row(view, rewards[index], prepared, reason);
        if (row != RewardPreparation::prepared) {
            return row;
        }
        mutation.rewards[index] = prepared;
    }
    return RewardPreparation::prepared;
}

} // namespace

namespace runtime::detail {

/** A quest value lives in the account or the selected character's objective values. */
std::int32_t& quest_value(unlocks::Table& table, const Quest& quest) noexcept {
    return quest.scope == Quest::Scope::account ? table.objectiveValues[quest.row]
                                                : table.characterObjectValues[quest.row];
}

/** A credited progression lives in the account or the selected character's progression bank. */
std::int32_t& progression_value(unlocks::Table& table,
                                const PreparedProgressionCredit& credit) noexcept {
    auto& bank = credit.scope == build_data::progressions::Scope::account
                     ? table.accountProgressions
                     : table.characterProgressions;
    return bank[credit.definitionIndex].front();
}

/** Overrides name socket types, not lanes, so adding an ordinary lane cannot shift a fixed roll. */
bool apply_reward_sockets(const item_details::Definition& detail,
                          std::span<const build_data::rewards::SocketOverride> overrides,
                          authored_inventory::Sockets& sockets,
                          const char** refusal) noexcept {
    const auto fail = [&](const char* reason) noexcept {
        if (refusal != nullptr) {
            *refusal = reason;
        }
        return false;
    };
    if (overrides.empty()) {
        return true;
    }
    if (detail.ordinarySocketCount > sockets.plugs.size()) {
        return fail("socket_layout");
    }
    authored_inventory::Sockets staged{};
    staged.policy = authored_inventory::SocketPolicy::authored;
    staged.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < staged.plugCount; ++lane) {
        const auto index = detail.initialPlugIndices[lane];
        if (index == item_details::kUnavailableItemIndex) {
            continue;
        }
        build_data::items::Definition plug{};
        if (!build_data::find_item_definition_index(index, plug)) {
            return fail("socket_layout");
        }
        staged.plugs[lane] = plug.definitionHash;
    }
    std::array<bool, item_details::kInitialPlugCapacity> replaced{};
    for (const auto& override : overrides) {
        if (override.plugSet != build_data::rewards::kAbsent) {
            return fail("socket_plug_set");
        }
        if (override.rollSet != build_data::rewards::kAbsent) {
            return fail("socket_roll_set");
        }
        // Zero selection preserves the initial plug in every lane of the named type.
        const bool preserve =
            override.plugItem == build_data::rewards::kAbsent && override.selection == 0;
        if (!preserve && override.selection != build_data::rewards::kFixedPlugSelection) {
            return fail("socket_selection");
        }
        build_data::items::Definition plug{};
        if (!preserve && !build_data::find_item_definition_index(override.plugItem, plug)) {
            return fail("socket_layout");
        }
        bool found = false;
        for (std::size_t lane = 0; lane < staged.plugCount; ++lane) {
            if (detail.socketTypes[lane] != override.socketType) {
                continue;
            }
            if (replaced[lane] || (!preserve && found)) {
                return fail("socket_layout");
            }
            replaced[lane] = true;
            found = true;
            if (!preserve) {
                staged.plugs[lane] = plug.definitionHash;
            }
        }
        if (!found) {
            return fail("socket_layout");
        }
    }
    if (!authored_inventory::valid(staged)) {
        return fail("socket_layout");
    }
    sockets = staged;
    return true;
}

/** Rows and flags read one account view, so the grant is one consistent after-image. */
RewardPreparation prepare_reward_grant(const RewardGrantRequest& request,
                                       PendingRecordRewardGrant& mutation,
                                       const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_count";
    mutation = {};
    const std::span<const DirectRecordReward> rewards = request.rewards;
    const bool flagsWrapper = request.wrapperFlags != nullptr && request.wrapperFlags->any();
    if ((rewards.empty() && !flagsWrapper && request.redeemedInstanceSoid == 0
         && !request.carriesSavedEffects)
        || rewards.size() > mutation.rewards.size()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    // The loadout check needs these domains, so nothing is decided before they publish.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::inventory_bucket_descriptors_ready()
        || !build_data::socket_entry_lists_ready()) {
        return RewardPreparation::notReady;
    }
    reason = "account_read";
    AccountState working{};
    if (!investment::store::read_account(working)) {
        return RewardPreparation::notReady;
    }
    reason = "account_state";
    const std::size_t characterIndex = selected_character_index(working);
    if (!account::valid(working) || !valid_profile_inventory(working)
        || characterIndex >= working.characterCount) {
        return RewardPreparation::deferred;
    }
    const auto flags = prepare_wrapper_flags(request.wrapperFlags, mutation, reason);
    if (flags != RewardPreparation::prepared) {
        return flags;
    }
    // Identities stay above the pre-eviction inventory, so a discarded resident is never reused.
    std::uint64_t nextInstanceSoid = 0;
    PlacedRows placed;
    reason = "instance_capacity";
    if (!next_item_instance_soid(working, nextInstanceSoid)) {
        return RewardPreparation::deferred;
    }
    record_grant_before(working, characterIndex, mutation);
    if (!remove_spent_instances(request, working.characters[characterIndex], mutation, reason)) {
        return RewardPreparation::deferred;
    }
    const auto spent =
        spend_price(request.price, request.nowSeconds, working, characterIndex, reason);
    if (spent != RewardPreparation::prepared) {
        return spent;
    }
    const GrantView view{working, characterIndex, {}, nextInstanceSoid, placed, request.nowSeconds};
    const auto rows = prepare_reward_rows(view, rewards, mutation, reason);
    if (rows != RewardPreparation::prepared) {
        return rows;
    }

    reason = "loadout";
    // A full native bucket rejects the whole loadout; retry later.
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || !family4_loadout::resolve(working, characterIndex, loadout)) {
        return RewardPreparation::deferred;
    }
    record_grant_after(working, placed, mutation);
    mutation.claimedRecordIndex = request.claimedRecordIndex;
    mutation.rewardCount = rewards.size();
    reason = nullptr;
    return RewardPreparation::prepared;
}

} // namespace runtime::detail

/**
 * Applies a prepared grant's flags, quest starts, completion values, quest step, progression
 * credit, ingredient credits, chooser readiness and seasonal XP to the saved banks.
 * @param after Receives the selected character's unlocks with the grant applied.
 * @return False when a flag, quest value, progression, ingredient balance, chooser row or the
 *         seasonal XP total moved since the grant was prepared.
 */
bool preview_reward_unlocks(const PendingRecordRewardGrant& mutation,
                            unlocks::Table& after) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    if (!mutation.prepared || mutation.rewardCount > mutation.rewards.size()
        || !investment::store::read_unlocks(after, static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    const auto rewards = std::span(mutation.rewards).first(mutation.rewardCount);
    for (const auto& reward : rewards) {
        if (reward.acquiredFlag != build_data::rewards::kAbsent
            && (reward.acquiredFlag >= after.accountFlags.size()
                || after.accountFlags[reward.acquiredFlag] != reward.previousFlag)) {
            return false;
        }
        if (reward.quest.scope != Quest::Scope::none
            && quest_value(after, reward.quest) != reward.previousQuestValue) {
            return false;
        }
    }
    for (const PreparedQuestStep* move : mutation.saved_value_moves()) {
        if (move->next.scope != Quest::Scope::none
            && (!build_data::items::valid_bank_row(move->next.scope, move->next.row)
                || quest_value(after, move->next) != move->previousValue)) {
            return false;
        }
    }
    const PreparedProgressionCredit& progression = mutation.progression;
    const bool credits = progression.scope != build_data::progressions::Scope::unreplicated;
    if (credits
        && (progression.definitionIndex >= after.accountProgressions.size()
            || progression_value(after, progression) != progression.previousValue)) {
        return false;
    }
    const auto wrapperFlags = mutation.wrapper_flags();
    for (std::size_t i = 0; i < wrapperFlags.size(); ++i) {
        const auto& flag = wrapperFlags[i];
        if (flag.index >= after.accountFlags.size() || flag.previousValue > unlocks::kFlagSet
            || (i != 0 && wrapperFlags[i - 1].index >= flag.index)
            || after.accountFlags[flag.index] != flag.previousValue) {
            return false;
        }
    }
    for (const auto& flag : wrapperFlags) {
        after.accountFlags[flag.index] = unlocks::kFlagSet;
    }
    for (const auto& reward : rewards) {
        if (reward.acquiredFlag != build_data::rewards::kAbsent) {
            after.accountFlags[reward.acquiredFlag] = unlocks::kFlagSet;
        }
        if (reward.quest.scope != Quest::Scope::none) {
            auto& value = quest_value(after, reward.quest);
            value = build_data::items::initialized_value(reward.quest, value);
        }
    }
    const VendorRankRewardClaim& claim = mutation.vendorReward;
    if (claim.beforeCredits != 0) {
        if (claim.rankCreditRow >= after.characterObjectValues.size()
            || !vendor_rank_reward_current(claim)
            || after.characterObjectValues[claim.rankCreditRow] != claim.beforeCredits) {
            return false;
        }
        after.characterObjectValues[claim.rankCreditRow] = claim.beforeCredits - 1;
    }
    for (const PreparedQuestStep* move : mutation.saved_value_moves()) {
        if (move->next.scope != Quest::Scope::none) {
            quest_value(after, move->next) = move->next.value;
        }
    }
    if (credits) {
        progression_value(after, progression) = progression.value;
    }
    // Pickups of one balance chain, so each observes the value the one before it left.
    const auto apply = [&after](const std::optional<AccountUnlockWrite>& write) noexcept {
        if (!write) {
            return true;
        }
        if (write->index >= after.objectiveValues.size()
            || after.objectiveValues[write->index] != write->previousValue) {
            return false;
        }
        after.objectiveValues[write->index] = write->value;
        return true;
    };
    for (const auto& reward : rewards) {
        if (!apply(reward.ingredientCredit) || !apply(reward.chooserReadiness)) {
            return false;
        }
    }
    // The commit grants the XP, after this image has gone out in the grant's frame.
    return mutation.seasonalExperience == 0
           || preview_seasonal_experience(
               mutation.seasonalExperienceBefore, mutation.seasonalExperience, after);
}

/** A reward with no acquisition flag leaves its collectible state unknown, so it marks nothing. */
bool newly_collected(const PendingRecordRewardGrant& mutation,
                     const GrantedRow& row,
                     const build_data::items::Definition& item) noexcept {
    if (!row.newRow || mutation.rewardCount > mutation.rewards.size()
        || !build_data::collectibles::grants_item(item.definitionIndex)) {
        return false;
    }
    const auto rewards = std::span(mutation.rewards).first(mutation.rewardCount);
    return std::any_of(rewards.begin(), rewards.end(), [&](const PreparedRecordReward& reward) {
        return reward.definitionHash == item.definitionHash
               && reward.acquiredFlag != build_data::rewards::kAbsent
               && reward.previousFlag != unlocks::kFlagSet;
    });
}

/** Diffs the grant's images by instance identity, new character instances first. */
bool granted_residents(const PendingRecordRewardGrant& mutation,
                       GrantedResidents& output) noexcept {
    output = {};
    const auto add = [](auto& soids, std::size_t& count, std::uint64_t soid) noexcept {
        if (count == soids.size()) {
            return false;
        }
        soids[count++] = soid;
        return true;
    };
    const CharacterState& before = mutation.beforeCharacter;
    const CharacterState& after = mutation.afterCharacter;
    if (before.inventory.count > before.inventory.values.size()
        || after.inventory.count > after.inventory.values.size()) {
        return false;
    }
    // A FIFO bucket counts its equipped rows, so an eviction can move or discard one.
    const auto holds = [](const CharacterState& character, std::uint64_t soid) noexcept {
        const auto held = std::span(character.inventory.values).first(character.inventory.count);
        const auto& slots = character.equipment.slots;
        return std::any_of(held.begin(),
                           held.end(),
                           [&](const auto& item) noexcept { return item.instanceSoid == soid; })
               || std::any_of(slots.begin(), slots.end(), [&](const auto& item) noexcept {
                      return item && item->instanceSoid == soid;
                  });
    };
    for (std::size_t index = 0; index < after.inventory.count; ++index) {
        const std::uint64_t soid = after.inventory.values[index].instanceSoid;
        if (!holds(before, soid) && !add(output.soids, output.count, soid)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        const std::uint64_t soid = before.inventory.values[index].instanceSoid;
        if (!holds(after, soid) && !add(output.released, output.releasedCount, soid)) {
            return false;
        }
    }
    for (const auto& equipped : before.equipment.slots) {
        if (equipped && !holds(after, equipped->instanceSoid)
            && !add(output.released, output.releasedCount, equipped->instanceSoid)) {
            return false;
        }
    }
    const auto priorProfile =
        std::span(mutation.beforeProfileItems).first(mutation.beforeProfileItemCount);
    for (std::size_t index = 0; index < mutation.afterProfileItemCount; ++index) {
        const std::uint64_t soid = mutation.afterProfileItems[index].instanceSoid;
        if (soid != 0
            && std::none_of(priorProfile.begin(),
                            priorProfile.end(),
                            [&](const auto& item) noexcept { return item.instanceSoid == soid; })
            && !add(output.soids, output.count, soid)) {
            return false;
        }
    }
    return true;
}

/** Compares the unequipped copies by instance identity. */
bool item_values_edited(const CharacterState& before,
                        const CharacterState& after,
                        std::uint64_t instanceSoid) noexcept {
    const auto find = [instanceSoid](const CharacterState& character) noexcept {
        const auto held = std::span(character.inventory.values).first(character.inventory.count);
        const auto found = std::find_if(held.begin(), held.end(), [&](const auto& item) noexcept {
            return item.instanceSoid == instanceSoid;
        });
        return found != held.end() ? &*found : nullptr;
    };
    const authored_inventory::Item* earlier = find(before);
    const authored_inventory::Item* later = find(after);
    return instanceSoid != 0 && earlier != nullptr && later != nullptr
           && earlier->objectives != later->objectives;
}

/** Prepares every direct reward over one cumulative account view. */
RewardPreparation prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                              std::uint16_t claimedRecordIndex,
                                              std::int64_t nowSeconds,
                                              PendingRecordRewardGrant& mutation,
                                              const char** refusal) noexcept {
    const RewardGrantRequest request{
        .rewards = rewards, .claimedRecordIndex = claimedRecordIndex, .nowSeconds = nowSeconds};
    return prepare_reward_grant(request, mutation, refusal);
}

/** Builds the full account after-image while a record reward remains current. */
bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                 AccountState& after) noexcept {
    const std::lock_guard lock(investment::store::g_mutex);
    return investment::store::read_account(after) && materialize_record_reward(mutation, after);
}

namespace {

/** Logs each reward the client's overflow rules discarded, in whole or in part. */
void report_dropped_rewards(const PendingRecordRewardGrant& mutation) noexcept {
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        if (reward.placedQuantity < reward.quantity) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=reward_grant stage=commit result=dropped hash=0x%08X "
                              "requested=%d placed=%d",
                              static_cast<unsigned>(reward.definitionHash),
                              static_cast<int>(reward.quantity),
                              static_cast<int>(reward.placedQuantity));
        }
    }
}

/**
 * Grants a grant's seasonal XP inside the commit's transaction.
 * @param amount XP the grant pays; zero grants nothing.
 * @param bonusMoved Set when the new total changed the artifact Power bonus.
 * @return False when a nonzero amount could not be granted.
 */
[[nodiscard]] bool grant_reward_experience(std::int32_t amount, bool& bonusMoved) noexcept {
    if (amount == 0) {
        return true;
    }
    const std::uint16_t bonusBefore = artifact_power_bonus();
    if (!grant_seasonal_experience(amount)) {
        return false;
    }
    bonusMoved = artifact_power_bonus() != bonusBefore;
    return true;
}

} // namespace

/**
 * Commits the shared reward after-image, its flags, its quest starts, completion values and step,
 * its progression and ingredient credits, its chooser readiness, its claim and its seasonal XP
 * together.
 * @param mutation Prepared grant, consumed on every exit.
 * @param artifactBonusMoved Set when the grant's XP changed the artifact Power bonus; clear when
 *        nothing was committed.
 * @return False when the account or a saved value moved; a record claim is revoked then.
 */
bool commit_record_reward(PendingRecordRewardGrant& mutation, bool* artifactBonusMoved) noexcept {
    if (artifactBonusMoved != nullptr) {
        *artifactBonusMoved = false;
    }
    if (!mutation.prepared) {
        return false;
    }
    const PendingConsumption consume{mutation};
    bool ready = false;
    bool bonusMoved = false;
    {
        investment::store::Transaction transaction;
        AccountState after{};
        unlocks::Table flags{};
        ready = transaction.ready() && investment::store::read_account(after)
                && materialize_record_reward(mutation, after)
                && preview_reward_unlocks(mutation, flags)
                && investment::store::write_account(after);
        for (std::size_t i = 0; ready && i < mutation.rewardCount; ++i) {
            const auto& reward = mutation.rewards[i];
            if (reward.acquiredFlag != build_data::rewards::kAbsent) {
                ready = investment::store::write_unlock(
                    investment::store::Bank::accountFlags, reward.acquiredFlag, unlocks::kFlagSet);
            }
            // A quest under way keeps its step; only an unset one starts.
            if (ready && reward.quest.scope != Quest::Scope::none
                && reward.previousQuestValue == build_data::items::kUnsetQuestValue) {
                ready = investment::store::write_unlock(
                    quest_bank(reward.quest), reward.quest.row, reward.quest.value);
            }
            if (ready && reward.ingredientCredit) {
                ready = investment::store::write_unlock(investment::store::Bank::objectiveValues,
                                                        reward.ingredientCredit->index,
                                                        reward.ingredientCredit->value);
            }
            if (ready && reward.chooserReadiness) {
                ready = investment::store::write_unlock(investment::store::Bank::objectiveValues,
                                                        reward.chooserReadiness->index,
                                                        reward.chooserReadiness->value);
            }
        }
        for (const auto& flag : mutation.wrapper_flags()) {
            if (!ready) {
                break;
            }
            ready = investment::store::write_unlock(
                investment::store::Bank::accountFlags, flag.index, unlocks::kFlagSet);
        }
        if (ready && mutation.vendorReward.beforeCredits > 0) {
            ready = investment::store::write_unlock(investment::store::Bank::characterObjectValues,
                                                    mutation.vendorReward.rankCreditRow,
                                                    mutation.vendorReward.beforeCredits - 1);
        }
        for (const PreparedQuestStep* move : mutation.saved_value_moves()) {
            const Quest& value = move->next;
            if (ready && value.scope != Quest::Scope::none) {
                ready = investment::store::write_unlock(quest_bank(value), value.row, value.value);
            }
        }
        const PreparedProgressionCredit& progression = mutation.progression;
        if (ready && progression.scope != build_data::progressions::Scope::unreplicated) {
            ready = investment::store::write_unlock(
                progression.scope == build_data::progressions::Scope::account
                    ? investment::store::Bank::accountProgressions
                    : investment::store::Bank::characterProgressions,
                progression.definitionIndex,
                progression.value);
        }
        // The XP is granted last, onto the total preview_reward_unlocks checked above.
        ready = ready && grant_reward_experience(mutation.seasonalExperience, bonusMoved)
                && transaction.commit();
    }
    if (!ready && mutation.claimedRecordIndex != kUnclaimedRecordIndex) {
        // The claim was written when the reward was prepared, so a refused install undoes it.
        unlocks::records::revoke(mutation.claimedRecordIndex);
    }
    if (ready) {
        report_dropped_rewards(mutation);
    }
    if (artifactBonusMoved != nullptr) {
        *artifactBonusMoved = ready && bonusMoved;
    }
    return ready;
}

} // namespace sunrise::state
