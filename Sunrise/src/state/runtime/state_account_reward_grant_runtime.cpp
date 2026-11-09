/**
 * Record-reward grants: item rows, acquisition flags and quest starts committed as one transaction.
 * Triumph claims, Season pass claims and wrapper draws all prepare through here.
 */
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../../middleware/datagen/family4/account/layout.h"
#include "../../middleware/datagen/family4/character/layout.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../unlocks/unlocks_records.h"
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

/** Each native change list must fit independently; unlock flags consume neither list. */
[[nodiscard]] const char*
reward_change_refusal(std::span<const PreparedRecordReward> rewards) noexcept {
    std::size_t characterChanges = 0;
    std::size_t profileChanges = 0;
    for (const auto& reward : rewards) {
        switch (reward.kind) {
        case RecordRewardKind::characterInstance:
        case RecordRewardKind::characterStack:
            if (++characterChanges
                > middleware::datagen::family4::character::layout::kInventoryChangeRecordCapacity) {
                return "character_change_capacity";
            }
            break;
        case RecordRewardKind::profileStack:
            if (++profileChanges > middleware::datagen::family4::account::layout::
                    kProfileInventoryChangeRecordCapacity) {
                return "profile_change_capacity";
            }
            break;
        case RecordRewardKind::accountUnlock:
            break;
        default:
            return "reward_kind";
        }
    }
    return nullptr;
}

/** @return The saved value a prepared quest start reads in one unlock snapshot. */
[[nodiscard]] std::int32_t& quest_value(unlocks::Table& table, const Quest& quest) noexcept {
    return quest.scope == Quest::Scope::account ? table.objectiveValues[quest.row]
                                                : table.characterObjectValues[quest.row];
}

/**
 * Checks one prepared row against its own item rules and the rebuilt after-image.
 * @param mutation Grant the row belongs to.
 * @param reward Row to check.
 * @param loadout Loadout resolved from the after-image.
 * @return False when the row breaks a rule its kind carries.
 */
[[nodiscard]] bool reward_row_valid(const PendingRecordRewardGrant& mutation,
                                    const PreparedRecordReward& reward,
                                    const family4_loadout::ResolvedLoadout& loadout) noexcept {
    build_data::items::Definition item{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (reward.definitionHash == authored_inventory::kNoDefinitionHash || reward.quantity <= 0
        || reward.afterQuantity < reward.quantity || reward.mutationSerial < 0
        || !build_data::find_item_definition_hash(reward.definitionHash, item)
        || !build_data::items::valid(reward.quest)) {
        return false;
    }
    // Only a character instance starts a quest, and only the quest its definition names.
    if (reward.kind == RecordRewardKind::characterInstance
            ? reward.quest != item.questInitialization
            : reward.quest.scope != Quest::Scope::none) {
        return false;
    }
    if (reward.kind == RecordRewardKind::accountUnlock) {
        build_data::rewards::Item source{};
        return reward.quantity == 1 && reward.afterQuantity == 1 && reward.instanceSoid == 0
               && item.bucketId == build_data::inventory::buckets::kPerkBucketId
               && build_data::find_reward_item(item.definitionIndex, source)
               && source.acquiredFlag != build_data::rewards::kAbsent
               && source.acquiredFlag == reward.acquiredFlag;
    }
    if (!build_data::find_configured_item_detail(item.definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex
        || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return false;
    }
    if (reward.kind == RecordRewardKind::characterInstance) {
        if (reward.quantity != 1 || reward.afterQuantity != 1 || reward.instanceSoid == 0
            || reward.appendedProfileResident
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::instanced
            || bucket.arraySelector != inventory_buckets::ArraySelector::character
            || reward.stateIndex >= mutation.afterCharacter.inventory.count) {
            return false;
        }
        const auto& granted = mutation.afterCharacter.inventory.values[reward.stateIndex];
        std::uint16_t row = 0;
        std::uint8_t slot = 0;
        return granted.instanceSoid == reward.instanceSoid
               && granted.definitionHash == reward.definitionHash && granted.quantity == 1
               && granted.mutationSerial == reward.mutationSerial
               && find_unequipped_row(loadout, reward.instanceSoid, row, slot)
               && row == reward.inventoryRow;
    }
    if (reward.kind == RecordRewardKind::characterStack) {
        if (reward.instanceSoid != 0 || reward.appendedProfileResident
            || detail.equipmentSlot.has_value()
            || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
            || bucket.arraySelector != inventory_buckets::ArraySelector::character
            || reward.afterQuantity > detail.maxStackSize
            || reward.stateIndex >= mutation.afterCharacter.stacks.count) {
            return false;
        }
        const auto& granted = mutation.afterCharacter.stacks.values[reward.stateIndex];
        return granted.definitionHash == reward.definitionHash
               && granted.quantity == reward.afterQuantity
               && granted.mutationSerial == reward.mutationSerial;
    }
    if (reward.kind != RecordRewardKind::profileStack
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || bucket.arraySelector != inventory_buckets::ArraySelector::profile
        || reward.afterQuantity > detail.maxStackSize
        || reward.stateIndex >= mutation.afterProfileItemCount) {
        return false;
    }
    const auto& granted = mutation.afterProfileItems[reward.stateIndex];
    const bool actionSource =
        build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
    return granted.instanceSoid == reward.instanceSoid
           && granted.definitionHash == reward.definitionHash
           && granted.quantity == reward.afterQuantity
           && granted.mutationSerial == reward.mutationSerial
           && actionSource == (reward.instanceSoid != 0)
           && reward.appendedProfileResident
                  == (actionSource && reward.stateIndex >= mutation.beforeProfileItemCount);
}

/**
 * Rebuilds the account after-image for one prepared reward set and rejects any drift.
 * @param current Live account the mutation was prepared against.
 * @param after Receives the after-image; its content is unusable on failure.
 * @return False when the account moved or a reward row breaks its own item rules.
 */
[[nodiscard]] bool materialize_record_reward(const AccountState& current,
                                             const PendingRecordRewardGrant& mutation,
                                             AccountState& after) noexcept {
    if (!mutation.prepared || (mutation.rewardCount == 0 && mutation.wrapperFlagCount == 0)
        || mutation.rewardCount > mutation.rewards.size()
        || reward_change_refusal(std::span(mutation.rewards).first(mutation.rewardCount)) != nullptr
        || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.characterIndex >= current.characterCount
        || current.primarySoid != mutation.accountSoid
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)
        || !same_profile_inventory(
            current, mutation.beforeProfileItems, mutation.beforeProfileItemCount)
        || !mutation.beforeCharacter.selected
        || mutation.beforeCharacter.soid != mutation.characterSoid) {
        return false;
    }

    after = current;
    after.characters[mutation.characterIndex] = mutation.afterCharacter;
    after.profileItems = mutation.afterProfileItems;
    after.profileItemCount = mutation.afterProfileItemCount;
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(after) || !valid_profile_inventory(after)
        || !family4_loadout::resolve(after, mutation.characterIndex, loadout)) {
        return false;
    }
    for (std::size_t index = 0; index < mutation.rewardCount; ++index) {
        if (!reward_row_valid(mutation, mutation.rewards[index], loadout)) {
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
 *         deferred when a saved flag cannot be read.
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
            return RewardPreparation::deferred;
        }
        mutation.wrapperFlags[mutation.wrapperFlagCount++] = {static_cast<std::uint16_t>(index),
                                                              static_cast<std::uint8_t>(before)};
    }
    return RewardPreparation::prepared;
}

/**
 * Captures the acquisition flag a resolved reward row sets and the value it holds now.
 * @return Unresolvable when the row has no reward entry; deferred when the flag cannot be read.
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
    // The saved flag refuses here, not the definition.
    if (!investment::store::read_unlock(
            investment::store::Bank::accountFlags, source.acquiredFlag, before)
        || before < 0 || before > unlocks::kFlagSet) {
        return RewardPreparation::deferred;
    }
    prepared.previousFlag = static_cast<std::uint8_t>(before);
    return RewardPreparation::prepared;
}

/** The selected character's working view, shared by every row of one grant. */
struct GrantView {
    AccountState& working;
    std::size_t characterIndex{};
    std::span<const PreparedRecordReward> prior;
};

/**
 * Appends one profile stack to the working view.
 * @return Deferred when the profile inventory has no room for it.
 */
[[nodiscard]] RewardPreparation place_profile_stack(GrantView view,
                                                    const build_data::items::Definition& item,
                                                    const item_details::Definition& detail,
                                                    std::int32_t quantity,
                                                    PreparedRecordReward& prepared) noexcept {
    if (detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || quantity > detail.maxStackSize) {
        return RewardPreparation::unresolvable;
    }
    PendingProfileItemAcquisition staged{};
    const bool actionSource =
        build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
    if (!finalize_profile_item_acquisition(view.working,
                                           view.working,
                                           item.definitionHash,
                                           detail,
                                           actionSource,
                                           quantity,
                                           {.direct = true},
                                           staged)) {
        return RewardPreparation::deferred;
    }
    view.working.profileItems = staged.afterItems;
    view.working.profileItemCount = staged.afterItemCount;
    prepared.instanceSoid = staged.acquiredInstanceSoid;
    prepared.stateIndex = staged.profileIndex;
    prepared.afterQuantity = staged.acquiredQuantity;
    prepared.mutationSerial = staged.acquiredMutationSerial;
    prepared.kind = RecordRewardKind::profileStack;
    prepared.appendedProfileResident = staged.appended && staged.actionSource;
    return RewardPreparation::prepared;
}

/**
 * Appends one character instance, with its socket overrides and any quest start it carries.
 * @param reason Receives a static reason on failure.
 * @return Deferred when the inventory has no room; unresolvable for a bad override or quest.
 */
[[nodiscard]] RewardPreparation
place_character_instance(GrantView view,
                         const build_data::items::Definition& item,
                         const item_details::Definition& detail,
                         std::span<const build_data::rewards::SocketOverride> sockets,
                         PreparedRecordReward& prepared,
                         const char*& reason) noexcept {
    reason = "instance_capacity";
    PendingItemAcquisition staged{};
    if (!finalize_item_acquisition(
            view.working, view.working, item.definitionHash, false, {.direct = true}, staged)) {
        return RewardPreparation::deferred;
    }
    if (!apply_reward_sockets(detail,
                              sockets,
                              staged.afterCharacter.inventory.values[staged.inventoryIndex].sockets,
                              &reason)) {
        return RewardPreparation::unresolvable;
    }
    // Two rows that start one quest would each see it unset; the second start is refused.
    reason = "duplicate_quest";
    for (const auto& earlier : view.prior) {
        if (staged.questInitialization.scope != Quest::Scope::none
            && earlier.quest.scope == staged.questInitialization.scope
            && earlier.quest.row == staged.questInitialization.row) {
            return RewardPreparation::unresolvable;
        }
    }
    view.working.characters[view.characterIndex] = staged.afterCharacter;
    prepared.instanceSoid = staged.acquiredInstanceSoid;
    prepared.stateIndex = staged.inventoryIndex;
    prepared.afterQuantity = 1;
    prepared.mutationSerial =
        staged.afterCharacter.inventory.values[staged.inventoryIndex].mutationSerial;
    prepared.inventoryRow = staged.inventoryRow;
    prepared.kind = RecordRewardKind::characterInstance;
    prepared.quest = staged.questInitialization;
    prepared.previousQuestValue = staged.previousQuestValue;
    return RewardPreparation::prepared;
}

/**
 * Adds one stack of a stackable, non-equippable character item to the working view.
 * @return Deferred when the stack or the stack list has no room.
 */
[[nodiscard]] RewardPreparation place_character_stack(GrantView view,
                                                      const build_data::items::Definition& item,
                                                      const item_details::Definition& detail,
                                                      std::int32_t quantity,
                                                      PreparedRecordReward& prepared) noexcept {
    CharacterState& character = view.working.characters[view.characterIndex];
    if (quantity > detail.maxStackSize) {
        return RewardPreparation::unresolvable;
    }
    if (character.nextInventorySerial
        >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return RewardPreparation::deferred;
    }
    std::size_t stackIndex = character.stacks.count;
    for (std::size_t candidate = 0; candidate < character.stacks.count; ++candidate) {
        if (character.stacks.values[candidate].definitionHash == item.definitionHash) {
            stackIndex = candidate;
            break;
        }
    }
    const bool appended = stackIndex == character.stacks.count;
    if ((appended && stackIndex >= character.stacks.values.size())
        || (!appended
            && character.stacks.values[stackIndex].quantity > detail.maxStackSize - quantity)) {
        return RewardPreparation::deferred;
    }
    auto& stack = character.stacks.values[stackIndex];
    if (appended) {
        stack.definitionHash = item.definitionHash;
        ++character.stacks.count;
    }
    stack.quantity += quantity;
    stack.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    prepared.stateIndex = stackIndex;
    prepared.afterQuantity = stack.quantity;
    prepared.mutationSerial = stack.mutationSerial;
    prepared.kind = RecordRewardKind::characterStack;
    return RewardPreparation::prepared;
}

/**
 * Prepares one requested row against the working view and the rows before it.
 * @param reason Receives a static reason when the row is not prepared.
 * @return Deferred only when State may still make the row fit; unresolvable is final.
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
    // Only a character instance can start a quest, so any other destination refuses one.
    const bool quest = item.questInitialization.scope != Quest::Scope::none;
    if (item.bucketId == build_data::inventory::buckets::kPerkBucketId) {
        reason = "perk_acquisition";
        if (prepared.acquiredFlag == build_data::rewards::kAbsent || requested.quantity != 1
            || !requested.sockets.empty() || quest) {
            return RewardPreparation::unresolvable;
        }
        prepared.kind = RecordRewardKind::accountUnlock;
        prepared.afterQuantity = 1;
        return RewardPreparation::prepared;
    }
    reason = "item_detail";
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_configured_item_detail(requested.itemDefinitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex
        || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return RewardPreparation::unresolvable;
    }
    reason = "duplicate_stack";
    if (detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable) {
        for (const auto& earlier : view.prior) {
            if (earlier.definitionHash == item.definitionHash) {
                return RewardPreparation::unresolvable;
            }
        }
    }
    const bool character = bucket.arraySelector == inventory_buckets::ArraySelector::character;
    const bool instanced =
        detail.instancedDefinitionState == item_details::InstancedDefinitionState::instanced;
    reason = "socket_owner";
    if (!requested.sockets.empty() && (!character || !instanced)) {
        return RewardPreparation::unresolvable;
    }
    reason = "quest_destination";
    if (quest && (!character || !instanced)) {
        return RewardPreparation::unresolvable;
    }
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
        reason = "profile_capacity";
        return place_profile_stack(view, item, detail, requested.quantity, prepared);
    }
    if (character && instanced) {
        reason = "instance_quantity";
        if (requested.quantity != 1) {
            return RewardPreparation::unresolvable;
        }
        return place_character_instance(view, item, detail, requested.sockets, prepared, reason);
    }
    reason = "character_stack_capacity";
    if (character && !detail.equipmentSlot.has_value()) {
        return place_character_stack(view, item, detail, requested.quantity, prepared);
    }
    reason = "inventory_destination";
    return RewardPreparation::unresolvable;
}

} // namespace

namespace runtime::detail {

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
RewardPreparation
prepare_reward_grant(std::span<const DirectRecordReward> rewards,
                     std::uint16_t claimedRecordIndex,
                     const std::bitset<unlocks::kAccountFlagCapacity>* wrapperFlags,
                     PendingRecordRewardGrant& mutation,
                     const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_count";
    mutation = {};
    if ((rewards.empty() && (wrapperFlags == nullptr || wrapperFlags->none()))
        || rewards.size() > mutation.rewards.size()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    // The loadout check needs these domains, so nothing is decided before they publish.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::inventory_bucket_descriptors_ready()
        || !build_data::socket_entry_lists_ready()) {
        return RewardPreparation::deferred;
    }
    reason = "account_state";
    const AccountState account = bound_account_snapshot();
    const std::size_t characterIndex = selected_character_index(account);
    if (!account::valid(account) || !valid_profile_inventory(account)
        || characterIndex >= account.characterCount) {
        return RewardPreparation::deferred;
    }
    const auto flags = prepare_wrapper_flags(wrapperFlags, mutation, reason);
    if (flags != RewardPreparation::prepared) {
        return flags;
    }
    AccountState working = account;
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        const GrantView view{working, characterIndex, std::span(mutation.rewards).first(index)};
        PreparedRecordReward prepared{};
        const auto row = prepare_reward_row(view, rewards[index], prepared, reason);
        if (row != RewardPreparation::prepared) {
            return row;
        }
        mutation.rewards[index] = prepared;
    }

    // A draw that outgrows a native change list is refused like any oversized draw.
    reason = reward_change_refusal(std::span(mutation.rewards).first(rewards.size()));
    if (reason != nullptr) {
        return RewardPreparation::unresolvable;
    }
    reason = "loadout";
    // A full native bucket rejects the whole loadout; retry later.
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || !family4_loadout::resolve(working, characterIndex, loadout)) {
        return RewardPreparation::deferred;
    }
    mutation.beforeCharacter = account.characters[characterIndex];
    mutation.afterCharacter = working.characters[characterIndex];
    mutation.beforeProfileItems = account.profileItems;
    mutation.afterProfileItems = working.profileItems;
    mutation.claimedRecordIndex = claimedRecordIndex;
    mutation.accountSoid = account.primarySoid;
    mutation.characterSoid = account.characters[characterIndex].soid;
    mutation.characterIndex = characterIndex;
    mutation.beforeProfileItemCount = account.profileItemCount;
    mutation.afterProfileItemCount = working.profileItemCount;
    mutation.rewardCount = rewards.size();
    reason = nullptr;
    mutation.prepared = true;
    return RewardPreparation::prepared;
}

} // namespace runtime::detail

/**
 * Applies a prepared grant's flags and quest starts to the saved banks.
 * @param after Receives the selected character's unlocks with the grant applied.
 * @return False when a flag or quest value moved since the grant was prepared.
 */
bool preview_reward_unlocks(const PendingRecordRewardGrant& mutation,
                            unlocks::Table& after) noexcept {
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
    return true;
}

/** Prepares every direct reward over one cumulative account view. */
RewardPreparation prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                              std::uint16_t claimedRecordIndex,
                                              PendingRecordRewardGrant& mutation,
                                              const char** refusal) noexcept {
    return prepare_reward_grant(rewards, claimedRecordIndex, nullptr, mutation, refusal);
}

/** Builds the full account after-image while a record reward remains current. */
bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                 AccountState& after) noexcept {
    after = {};
    return materialize_record_reward(bound_account_snapshot(), mutation, after);
}

/**
 * Commits the shared reward after-image, its flags, its quest starts and its claim together.
 * @param mutation Prepared grant, consumed on every exit.
 * @return False when the account or a saved value moved; a record claim is revoked then.
 */
bool commit_record_reward(PendingRecordRewardGrant& mutation) noexcept {
    if (!mutation.prepared) {
        return false;
    }
    const PendingConsumption consume{mutation};
    bool ready = false;
    {
        investment::store::Transaction transaction;
        AccountState after{};
        unlocks::Table flags{};
        ready = transaction.ready()
                && materialize_record_reward(investment::store::account(), mutation, after)
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
        }
        for (const auto& flag : mutation.wrapper_flags()) {
            if (!ready) {
                break;
            }
            ready = investment::store::write_unlock(
                investment::store::Bank::accountFlags, flag.index, unlocks::kFlagSet);
        }
        ready = ready && transaction.commit();
    }
    if (!ready && mutation.claimedRecordIndex != kUnclaimedRecordIndex) {
        // The claim was written when the reward was prepared, so a refused install undoes it.
        unlocks::records::revoke(mutation.claimedRecordIndex);
    }
    return ready;
}

} // namespace sunrise::state
