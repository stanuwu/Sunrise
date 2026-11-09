/**
 * Record-reward grants: item rows, acquisition flags and quest starts committed as one transaction.
 * Triumph claims, Season pass claims and wrapper draws all prepare through here.
 */
#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "../../core/logging/log.h"
#include "../../middleware/datagen/family4/account/layout.h"
#include "../../middleware/datagen/family4/character/layout.h"
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

/** @return The kind of item a reward grants; overflow can still land a stack in its recovery
 * bucket. */
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
            && mutation.redeemedInstanceSoid == 0)
        || mutation.rewardCount > mutation.rewards.size()
        || mutation.rowCount > mutation.rows.size()
        || change_ring_refusal(std::span(mutation.rows).first(mutation.rowCount)) != nullptr
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
    // Only a character instance can start a quest, so any other destination refuses one.
    const bool quest = item.questInitialization.scope != Quest::Scope::none;
    if (item.bucketId == build_data::inventory::buckets::kPerkBucketId) {
        reason = "perk_acquisition";
        if (prepared.acquiredFlag == build_data::rewards::kAbsent || requested.quantity != 1
            || !requested.sockets.empty() || quest) {
            return RewardPreparation::unresolvable;
        }
        prepared.kind = RecordRewardKind::accountUnlock;
        prepared.placedQuantity = 1;
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
    prepared.kind = reward_kind(detail, bucket);
    const bool character = bucket.arraySelector == inventory_buckets::ArraySelector::character;
    const bool instance = prepared.kind == RecordRewardKind::characterInstance;
    reason = "socket_owner";
    if (!requested.sockets.empty() && !instance) {
        return RewardPreparation::unresolvable;
    }
    reason = "quest_destination";
    if (quest && !instance) {
        return RewardPreparation::unresolvable;
    }
    reason = "instance_quantity";
    if (instance && requested.quantity != 1) {
        return RewardPreparation::unresolvable;
    }
    reason = "inventory_destination";
    if ((!character && bucket.arraySelector != inventory_buckets::ArraySelector::profile)
        || (instance && !character) || (!instance && detail.equipmentSlot.has_value())) {
        return RewardPreparation::unresolvable;
    }
    Arrival arrival{item, detail, requested.quantity};
    if (instance) {
        arrival.instance.level = acquisition_level(view.working.characters[view.characterIndex]);
        arrival.instance.sockets.policy = authored_inventory::SocketPolicy::nativeDefaults;
        if (!apply_reward_sockets(detail, requested.sockets, arrival.instance.sockets, &reason)) {
            return RewardPreparation::unresolvable;
        }
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
    if (prepared.placedQuantity == 0) {
        return RewardPreparation::prepared;
    }
    // Only an item that found a row runs its acquisition side effects.
    if (instance) {
        prepared.instanceSoid = view.nextInstanceSoid - 1;
    }
    return quest ? prepare_quest_start(view, item, prepared, reason) : RewardPreparation::prepared;
}

} // namespace

namespace runtime::detail {

/** A quest value lives in the account or the selected character's objective values. */
std::int32_t& quest_value(unlocks::Table& table, const Quest& quest) noexcept {
    return quest.scope == Quest::Scope::account ? table.objectiveValues[quest.row]
                                                : table.characterObjectValues[quest.row];
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
RewardPreparation
prepare_reward_grant(std::span<const DirectRecordReward> rewards,
                     std::uint16_t claimedRecordIndex,
                     const std::bitset<unlocks::kAccountFlagCapacity>* wrapperFlags,
                     std::uint64_t redeemedInstanceSoid,
                     PendingRecordRewardGrant& mutation,
                     const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_count";
    mutation = {};
    if ((rewards.empty() && (wrapperFlags == nullptr || wrapperFlags->none())
         && redeemedInstanceSoid == 0)
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
    const auto flags = prepare_wrapper_flags(wrapperFlags, mutation, reason);
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
    // The pursuit leaves before the rows are placed, so its row is free for them.
    reason = "redeemed_instance";
    if (redeemedInstanceSoid != 0
        && !remove_redeemed(working.characters[characterIndex], redeemedInstanceSoid)) {
        return RewardPreparation::deferred;
    }
    mutation.redeemedInstanceSoid = redeemedInstanceSoid;
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        const GrantView view{working,
                             characterIndex,
                             std::span(mutation.rewards).first(index),
                             nextInstanceSoid,
                             placed};
        PreparedRecordReward prepared{};
        const auto row = prepare_reward_row(view, rewards[index], prepared, reason);
        if (row != RewardPreparation::prepared) {
            return row;
        }
        mutation.rewards[index] = prepared;
    }

    reason = "loadout";
    // A full native bucket rejects the whole loadout; retry later.
    family4_loadout::ResolvedLoadout loadout{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || !family4_loadout::resolve(working, characterIndex, loadout)) {
        return RewardPreparation::deferred;
    }
    record_grant_after(working, placed, mutation);
    mutation.claimedRecordIndex = claimedRecordIndex;
    mutation.rewardCount = rewards.size();
    reason = nullptr;
    return RewardPreparation::prepared;
}

} // namespace runtime::detail

/**
 * Applies a prepared grant's flags, quest starts and quest step to the saved banks.
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
    const PreparedQuestStep& step = mutation.questStep;
    if (step.next.scope != Quest::Scope::none
        && (!build_data::items::valid_bank_row(step.next.scope, step.next.row)
            || quest_value(after, step.next) != step.previousValue)) {
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
    if (step.next.scope != Quest::Scope::none) {
        quest_value(after, step.next) = step.next.value;
    }
    return true;
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

/** Prepares every direct reward over one cumulative account view. */
RewardPreparation prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                              std::uint16_t claimedRecordIndex,
                                              PendingRecordRewardGrant& mutation,
                                              const char** refusal) noexcept {
    return prepare_reward_grant(rewards, claimedRecordIndex, nullptr, 0, mutation, refusal);
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

} // namespace

/**
 * Commits the shared reward after-image, its flags, its quest starts and step, and its claim
 * together.
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
        }
        for (const auto& flag : mutation.wrapper_flags()) {
            if (!ready) {
                break;
            }
            ready = investment::store::write_unlock(
                investment::store::Bank::accountFlags, flag.index, unlocks::kFlagSet);
        }
        const Quest& step = mutation.questStep.next;
        if (ready && step.scope != Quest::Scope::none) {
            ready = investment::store::write_unlock(quest_bank(step), step.row, step.value);
        }
        ready = ready && transaction.commit();
    }
    if (!ready && mutation.claimedRecordIndex != kUnclaimedRecordIndex) {
        // The claim was written when the reward was prepared, so a refused install undoes it.
        unlocks::records::revoke(mutation.claimedRecordIndex);
    }
    if (ready) {
        report_dropped_rewards(mutation);
    }
    return ready;
}

} // namespace sunrise::state
