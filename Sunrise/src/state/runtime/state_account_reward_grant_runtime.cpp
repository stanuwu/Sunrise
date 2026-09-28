/**
 * Grants that no purchase pays for: season pass rewards, record rewards, and the
 * default emote collection.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../../middleware/datagen/family4/account/layout.h"
#include "../../middleware/datagen/family4/character/layout.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
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

namespace {

/** Prepares inventory rows and consumed-wrapper flags against the same account view. */
[[nodiscard]] RewardPreparation
prepare_reward_grant(std::span<const DirectRecordReward> rewards,
                     std::uint16_t claimedRecordIndex,
                     const std::bitset<unlocks::kAccountFlagCapacity>* wrapperFlags,
                     PendingRecordRewardGrant& mutation,
                     const char** refusal) noexcept;

[[nodiscard]] bool materialize_record_reward(const AccountState& current,
                                             const PendingRecordRewardGrant& mutation,
                                             AccountState& after) noexcept;

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

/** A reward overrides socket types, so adding an ordinary lane cannot shift a fixed roll. */
[[nodiscard]] bool
apply_reward_sockets(const item_details::Definition& detail,
                     std::span<const build_data::rewards::SocketOverride> overrides,
                     authored_inventory::Sockets& sockets,
                     const char** refusal = nullptr) noexcept {
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

[[nodiscard]] RewardPreparation prepare_resolved_reward(const reward_resolution::Result& resolved,
                                                        PendingRecordRewardGrant& mutation,
                                                        const char** refusal) noexcept {
    std::array<DirectRecordReward, kRecordRewardGrantCapacity> rows{};
    if (resolved.count > rows.size()) {
        return RewardPreparation::unresolvable;
    }
    for (std::size_t i = 0; i < resolved.count; ++i) {
        const auto& grant = resolved.grants[i];
        rows[i] = {grant.itemIndex,
                   grant.quantity,
                   std::span(grant.sockets).first(grant.socketCount),
                   true};
    }
    return prepare_reward_grant(std::span(rows).first(resolved.count),
                                kUnclaimedRecordIndex,
                                &resolved.wrapperFlags,
                                mutation,
                                refusal);
}

/** Finds the kept unsocketed grant of this item, or null. */
[[nodiscard]] reward_resolution::Grant* kept_stack(reward_resolution::Result& resolved,
                                                   std::size_t kept,
                                                   std::uint16_t itemIndex) noexcept {
    for (std::size_t prior = 0; prior < kept; ++prior) {
        auto& grant = resolved.grants[prior];
        if (grant.itemIndex == itemIndex && grant.socketCount == 0) {
            return &grant;
        }
    }
    return nullptr;
}

/** Folds repeat draws of a stackable item into its first draw; false when one stack overflows. */
[[nodiscard]] bool consolidate_stacks(reward_resolution::Result& resolved) noexcept {
    std::size_t kept = 0;
    for (std::size_t index = 0; index < resolved.count; ++index) {
        const reward_resolution::Grant grant = resolved.grants[index];
        item_details::Definition detail{};
        const bool stackable =
            grant.socketCount == 0
            && build_data::find_configured_item_detail(grant.itemIndex, detail)
            && detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable;
        auto* const merged = stackable ? kept_stack(resolved, kept, grant.itemIndex) : nullptr;
        if (merged == nullptr) {
            resolved.grants[kept++] = grant;
            continue;
        }
        if (static_cast<std::int64_t>(merged->quantity) + grant.quantity > detail.maxStackSize) {
            return false;
        }
        merged->quantity += grant.quantity;
    }
    resolved.count = kept;
    return true;
}

/** Classifies a catalog refusal: a verdict unless a catalog was cleared meanwhile. */
[[nodiscard]] RewardPreparation catalog_refusal(const char*& reason) noexcept {
    if (build_data::item_definitions_ready() && build_data::reward_definitions_ready()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    return RewardPreparation::deferred;
}

/** Wrappers and direct account perks bypass quest initialization. */
bool uses_reward_definition(const build_data::items::Definition& item) noexcept {
    build_data::rewards::Item reward{};
    if (!build_data::find_reward_item(item.definitionIndex, reward)) {
        return false;
    }
    return reward.poolIndex != build_data::rewards::kAbsent
           || (item.bucketId == inventory_buckets::kPerkBucketId
               && reward.acquiredFlag != build_data::rewards::kAbsent);
}

enum class PassResolution { claim, replay };

[[nodiscard]] bool resolve_pass(const build_data::season_pass::Reward& reward,
                                const AccountState& account,
                                std::uint64_t seed,
                                reward_resolution::Result& result,
                                PassResolution resolution,
                                const char** reason = nullptr) noexcept {
    const auto character = selected_character_index(account);
    unlocks::Table flags{};
    build_data::rewards::Item source{};
    if (character >= account.characterCount
        || !investment::store::read_unlocks(flags, static_cast<int>(character))
        || !build_data::find_reward_item(reward.itemIndex, source)
        || source.definitionHash != reward.itemHash || reward.socketCount > reward.sockets.size()
        || reward.conditionCount > reward.condition.size()) {
        return false;
    }
    if (reason != nullptr) {
        *reason = "reward_condition";
    }
    if (resolution == PassResolution::replay && reward.claimFlagIndex < flags.accountFlags.size()) {
        flags.accountFlags[reward.claimFlagIndex] = unlocks::kFlagClear;
    }
    bool enabled = false;
    if (!reward_resolution::eligible(
            std::span(reward.condition).first(reward.conditionCount),
            {flags, account.characters[character].characterClass, seed, reason},
            enabled)
        || !enabled) {
        return false;
    }
    if (reward_resolution::resolve(
            {flags, account.characters[character].characterClass, seed, reason},
            reward.itemIndex,
            reward.quantity,
            result)
        != reward_resolution::Resolution::resolved) {
        return false;
    }
    if (!consolidate_stacks(result)) {
        if (reason != nullptr) {
            *reason = "stack_capacity";
        }
        return false;
    }
    if (reward.socketCount != 0) {
        if (result.count != 1 || result.grants[0].itemIndex != reward.itemIndex) {
            if (reason != nullptr) {
                *reason = "socket_owner";
            }
            return false;
        }
        result.grants[0].sockets = reward.sockets;
        result.grants[0].socketCount = reward.socketCount;
    }
    return true;
}

[[nodiscard]] bool reward_matches(const build_data::season_pass::Reward& reward,
                                  const PendingSeasonPassReward& mutation) noexcept {
    const auto* grant = &mutation.grant;
    reward_resolution::Result expected{};
    if (!mutation.prepared || mutation.sourceDefinitionHash != reward.itemHash
        || !resolve_pass(
            reward, account_snapshot(), mutation.seed, expected, PassResolution::replay)
        || expected.count != grant->rewardCount
        || expected.wrapperFlags.count() != grant->wrapperFlags.size()) {
        return false;
    }
    for (const auto& flag : grant->wrapperFlags) {
        if (flag.index >= expected.wrapperFlags.size() || !expected.wrapperFlags[flag.index]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < expected.count; ++i) {
        const auto& planned = expected.grants[i];
        const auto& prepared = grant->rewards[i];
        build_data::items::Definition definition{};
        build_data::rewards::Item source{};
        if (!build_data::find_item_definition_index(planned.itemIndex, definition)
            || !build_data::find_reward_item(planned.itemIndex, source)
            || prepared.definitionHash != definition.definitionHash
            || prepared.quantity != planned.quantity
            || prepared.acquiredFlag != source.acquiredFlag) {
            return false;
        }
        if (planned.socketCount != 0) {
            item_details::Definition detail{};
            authored_inventory::Sockets sockets{};
            if (prepared.kind != RecordRewardKind::characterInstance
                || prepared.stateIndex >= grant->afterCharacter.inventory.count
                || !build_data::find_configured_item_detail(planned.itemIndex, detail)
                || !apply_reward_sockets(
                    detail, std::span(planned.sockets).first(planned.socketCount), sockets)) {
                return false;
            }
            const auto& installed =
                grant->afterCharacter.inventory.values[prepared.stateIndex].sockets;
            if (installed.policy != sockets.policy || installed.plugCount != sockets.plugCount
                || installed.plugs != sockets.plugs) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool prepare_season_pass_reward(std::uint16_t rewardIndex,
                                std::uint64_t seed,
                                PendingSeasonPassReward& mutation,
                                const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "reward_index";
    mutation = {};
    const std::lock_guard lock(investment::store::g_mutex);
    build_data::season_pass::Reward reward{};
    if (!build_data::find_season_pass_reward(rewardIndex, reward)) {
        return false;
    }
    reason = "rank";
    if (reward.requiredRank > seasonal_rank()) {
        return false;
    }
    reason = "already_claimed";
    if (season_pass_reward_claimed(rewardIndex)) {
        return false;
    }
    mutation.seed = seed;
    reward_resolution::Result resolved{};
    reason = "reward_condition";
    if (!resolve_pass(
            reward, account_snapshot(), mutation.seed, resolved, PassResolution::claim, &reason)) {
        return false;
    }
    reason = "reward_placement";
    if (prepare_resolved_reward(resolved, mutation.grant, &reason) != RewardPreparation::prepared) {
        return false;
    }
    reason = nullptr;
    mutation.sourceDefinitionHash = reward.itemHash;
    mutation.rewardIndex = rewardIndex;
    mutation.prepared = true;
    return true;
}

ItemGrantRoute item_grant_route(std::uint16_t itemIndex) noexcept {
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return ItemGrantRoute::unavailable;
    }
    if (uses_reward_definition(item)) {
        return ItemGrantRoute::reward;
    }
    if (item.questInitialization.scope != build_data::items::QuestInitialization::Scope::none) {
        return ItemGrantRoute::quest;
    }
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_configured_item_detail(itemIndex, detail)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return ItemGrantRoute::unavailable;
    }
    if (bucket.arraySelector == inventory_buckets::ArraySelector::profile
        && detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable) {
        return ItemGrantRoute::profile;
    }
    return bucket.arraySelector == inventory_buckets::ArraySelector::character
               ? ItemGrantRoute::reward
               : ItemGrantRoute::unavailable;
}

RewardPreparation prepare_item_reward(std::uint16_t itemIndex,
                                      std::uint32_t quantity,
                                      std::uint64_t seed,
                                      PendingRecordRewardGrant& mutation,
                                      const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "not_ready";
    mutation = {};
    // Unpublished domains decide nothing; retry later.
    if (!build_data::item_definitions_ready() || !build_data::reward_definitions_ready()) {
        return RewardPreparation::deferred;
    }
    reason = "item_definition";
    const std::lock_guard lock(investment::store::g_mutex);
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return catalog_refusal(reason);
    }
    reason = "selected_character";
    const AccountState account = account_snapshot();
    const auto character = selected_character_index(account);
    unlocks::Table flags{};
    if (character >= account.characterCount
        || !investment::store::read_unlocks(flags, static_cast<int>(character))) {
        return RewardPreparation::deferred;
    }
    reward_resolution::Result resolved{};
    const auto resolution = reward_resolution::resolve(
        {flags, account.characters[character].characterClass, seed, &reason},
        itemIndex,
        quantity,
        resolved);
    // Unlocks can change and another draw can fit, so these are retried, not retired.
    if (resolution == reward_resolution::Resolution::ineligible
        || resolution == reward_resolution::Resolution::exceeded) {
        return RewardPreparation::deferred;
    }
    if (resolution != reward_resolution::Resolution::resolved) {
        return catalog_refusal(reason);
    }
    reason = "stack_capacity";
    if (!consolidate_stacks(resolved)) {
        return RewardPreparation::deferred;
    }
    reason = "reward_placement";
    return prepare_resolved_reward(resolved, mutation, &reason);
}

bool preview_reward_unlocks(const PendingRecordRewardGrant& mutation,
                            unlocks::Table& after) noexcept {
    if (!mutation.prepared || mutation.rewardCount > mutation.rewards.size()
        || !investment::store::read_unlocks(after, static_cast<int>(mutation.characterIndex))) {
        return false;
    }
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto& reward = mutation.rewards[i];
        if (reward.acquiredFlag == build_data::rewards::kAbsent) {
            continue;
        }
        if (reward.acquiredFlag >= after.accountFlags.size()
            || after.accountFlags[reward.acquiredFlag] != reward.previousFlag) {
            return false;
        }
    }
    for (std::size_t i = 0; i < mutation.wrapperFlags.size(); ++i) {
        const auto& flag = mutation.wrapperFlags[i];
        if (flag.index >= after.accountFlags.size() || flag.previousValue > unlocks::kFlagSet
            || (i != 0 && mutation.wrapperFlags[i - 1].index >= flag.index)
            || after.accountFlags[flag.index] != flag.previousValue) {
            return false;
        }
    }
    for (const auto& flag : mutation.wrapperFlags) {
        after.accountFlags[flag.index] = unlocks::kFlagSet;
    }
    for (std::size_t i = 0; i < mutation.rewardCount; ++i) {
        const auto flag = mutation.rewards[i].acquiredFlag;
        if (flag != build_data::rewards::kAbsent) {
            after.accountFlags[flag] = unlocks::kFlagSet;
        }
    }
    return true;
}

/** Revalidates the prepared reward under the lock that commits its inventory and flags. */
bool commit_season_pass_reward(PendingSeasonPassReward& mutation) noexcept {
    if (!mutation.prepared) {
        return false;
    }
    const PendingConsumption consume{mutation};
    bool ready = false;
    {
        investment::store::Transaction transaction;
        build_data::season_pass::Reward reward{};
        ready = transaction.ready()
                && build_data::find_season_pass_reward(mutation.rewardIndex, reward)
                && season_pass_reward_claimed(mutation.rewardIndex)
                && reward.requiredRank <= seasonal_rank() && reward_matches(reward, mutation)
                && commit_record_reward(mutation.grant) && transaction.commit();
    }
    if (!ready) {
        revoke_season_pass_reward(mutation.rewardIndex);
    }
    return ready;
}

namespace {

/**
 * Rebuilds the account after-image for one prepared reward set and rejects any drift.
 * @param current Live account the mutation was prepared against.
 * @param after Receives the after-image; its content is unusable on failure.
 * @return False when the account moved or a reward row breaks its own item rules.
 */
[[nodiscard]] bool materialize_record_reward(const AccountState& current,
                                             const PendingRecordRewardGrant& mutation,
                                             AccountState& after) noexcept {
    if (!mutation.prepared || (mutation.rewardCount == 0 && mutation.wrapperFlags.empty())
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
        const PreparedRecordReward& reward = mutation.rewards[index];
        build_data::items::Definition item{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (reward.definitionHash == authored_inventory::kNoDefinitionHash || reward.quantity <= 0
            || reward.afterQuantity < reward.quantity || reward.mutationSerial < 0
            || !build_data::find_item_definition_hash(reward.definitionHash, item)) {
            return false;
        }
        if (reward.kind == RecordRewardKind::accountUnlock) {
            build_data::rewards::Item source{};
            if (reward.quantity != 1 || reward.afterQuantity != 1 || reward.instanceSoid != 0
                || item.bucketId != build_data::inventory::buckets::kPerkBucketId
                || !build_data::find_reward_item(item.definitionIndex, source)
                || source.acquiredFlag == build_data::rewards::kAbsent
                || source.acquiredFlag != reward.acquiredFlag) {
                return false;
            }
            continue;
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
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::instanced
                || bucket.arraySelector != inventory_buckets::ArraySelector::character
                || reward.stateIndex >= mutation.afterCharacter.inventory.count) {
                return false;
            }
            const auto& granted = mutation.afterCharacter.inventory.values[reward.stateIndex];
            std::uint16_t row = 0;
            std::uint8_t slot = 0;
            if (granted.instanceSoid != reward.instanceSoid
                || granted.definitionHash != reward.definitionHash || granted.quantity != 1
                || granted.mutationSerial != reward.mutationSerial
                || !find_unequipped_row(loadout, reward.instanceSoid, row, slot)
                || row != reward.inventoryRow) {
                return false;
            }
        } else if (reward.kind == RecordRewardKind::characterStack) {
            if (reward.instanceSoid != 0 || reward.appendedProfileResident
                || detail.equipmentSlot.has_value()
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::stackable
                || bucket.arraySelector != inventory_buckets::ArraySelector::character
                || reward.afterQuantity > detail.maxStackSize
                || reward.stateIndex >= mutation.afterCharacter.stacks.count) {
                return false;
            }
            const auto& granted = mutation.afterCharacter.stacks.values[reward.stateIndex];
            if (granted.definitionHash != reward.definitionHash
                || granted.quantity != reward.afterQuantity
                || granted.mutationSerial != reward.mutationSerial) {
                return false;
            }
        } else {
            if (reward.kind != RecordRewardKind::profileStack
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::stackable
                || bucket.arraySelector != inventory_buckets::ArraySelector::profile
                || reward.afterQuantity > detail.maxStackSize
                || reward.stateIndex >= mutation.afterProfileItemCount) {
                return false;
            }
            const auto& granted = mutation.afterProfileItems[reward.stateIndex];
            const bool actionSource =
                build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
            if (granted.instanceSoid != reward.instanceSoid
                || granted.definitionHash != reward.definitionHash
                || granted.quantity != reward.afterQuantity
                || granted.mutationSerial != reward.mutationSerial
                || actionSource != (reward.instanceSoid != 0)
                || reward.appendedProfileResident
                       != (actionSource && reward.stateIndex >= mutation.beforeProfileItemCount)) {
                return false;
            }
        }
    }
    return true;
}

/** Captures only flags carried by consumed wrappers; storage failure remains retryable. */
[[nodiscard]] bool prepare_wrapper_flags(const std::bitset<unlocks::kAccountFlagCapacity>* flags,
                                         PendingRecordRewardGrant& mutation,
                                         const char*& reason) noexcept {
    if (flags == nullptr) {
        return true;
    }
    try {
        mutation.wrapperFlags.reserve(flags->count());
        for (std::size_t index = 0; index < flags->size(); ++index) {
            if (!(*flags)[index]) {
                continue;
            }
            reason = "wrapper_acquisition_flag";
            std::int32_t before = 0;
            if (!investment::store::read_unlock(investment::store::Bank::accountFlags,
                                                static_cast<std::uint16_t>(index),
                                                before)
                || before < 0 || before > unlocks::kFlagSet) {
                return false;
            }
            mutation.wrapperFlags.push_back(
                {static_cast<std::uint16_t>(index), static_cast<std::uint8_t>(before)});
        }
    } catch (...) {
        reason = "wrapper_flag_storage";
        return false;
    }
    return true;
}

/** Prepares inventory rows and consumed-wrapper flags against the same account view. */
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
    const AccountState account = account_snapshot();
    const std::size_t characterIndex = selected_character_index(account);
    if (!account::valid(account) || !valid_profile_inventory(account)
        || characterIndex >= account.characterCount) {
        return RewardPreparation::deferred;
    }

    if (!prepare_wrapper_flags(wrapperFlags, mutation, reason)) {
        return RewardPreparation::deferred;
    }
    AccountState working = account;
    for (std::size_t index = 0; index < rewards.size(); ++index) {
        reason = "item_identity";
        const DirectRecordReward& requested = rewards[index];
        build_data::items::Definition item{};
        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        if (requested.quantity <= 0
            || !build_data::find_item_definition_index(requested.itemDefinitionIndex, item)
            || item.questInitialization.scope
                   != build_data::items::QuestInitialization::Scope::none) {
            return RewardPreparation::unresolvable;
        }
        PreparedRecordReward prepared{};
        prepared.definitionHash = item.definitionHash;
        prepared.quantity = requested.quantity;
        if (requested.acquireUnlock) {
            reason = "acquisition_flag";
            // Only a resolved reward asks for its flag, so the graph is published.
            build_data::rewards::Item source{};
            if (!build_data::find_reward_item(item.definitionIndex, source)
                || source.definitionHash != item.definitionHash) {
                return RewardPreparation::unresolvable;
            }
            prepared.acquiredFlag = source.acquiredFlag;
            if (source.acquiredFlag != build_data::rewards::kAbsent) {
                std::int32_t before = 0;
                // The saved flag refuses here, not the definition.
                if (!investment::store::read_unlock(
                        investment::store::Bank::accountFlags, source.acquiredFlag, before)
                    || before < 0 || before > unlocks::kFlagSet) {
                    return RewardPreparation::deferred;
                }
                prepared.previousFlag = static_cast<std::uint8_t>(before);
            }
        }
        if (item.bucketId == build_data::inventory::buckets::kPerkBucketId) {
            reason = "perk_acquisition";
            if (prepared.acquiredFlag == build_data::rewards::kAbsent || requested.quantity != 1
                || !requested.sockets.empty()) {
                return RewardPreparation::unresolvable;
            }
            prepared.kind = RecordRewardKind::accountUnlock;
            prepared.afterQuantity = 1;
            mutation.rewards[index] = prepared;
            continue;
        }
        reason = "item_detail";
        if (!build_data::find_configured_item_detail(requested.itemDefinitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
            return RewardPreparation::unresolvable;
        }
        reason = "duplicate_stack";
        if (detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable) {
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (mutation.rewards[prior].definitionHash == item.definitionHash) {
                    return RewardPreparation::unresolvable;
                }
            }
        }

        reason = "socket_owner";
        if (!requested.sockets.empty()
            && (bucket.arraySelector != inventory_buckets::ArraySelector::character
                || detail.instancedDefinitionState
                       != item_details::InstancedDefinitionState::instanced)) {
            return RewardPreparation::unresolvable;
        }
        reason = "inventory_destination";
        if (bucket.arraySelector == inventory_buckets::ArraySelector::profile) {
            reason = "profile_capacity";
            if (detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
                || requested.quantity > detail.maxStackSize) {
                return RewardPreparation::unresolvable;
            }
            PendingProfileItemAcquisition staged{};
            const bool actionSource =
                build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
            if (!finalize_profile_item_acquisition(working,
                                                   working,
                                                   item.definitionHash,
                                                   detail,
                                                   actionSource,
                                                   requested.quantity,
                                                   {.direct = true},
                                                   staged)) {
                return RewardPreparation::deferred;
            }
            working.profileItems = staged.afterItems;
            working.profileItemCount = staged.afterItemCount;
            prepared.instanceSoid = staged.acquiredInstanceSoid;
            prepared.stateIndex = staged.profileIndex;
            prepared.afterQuantity = staged.acquiredQuantity;
            prepared.mutationSerial = staged.acquiredMutationSerial;
            prepared.kind = RecordRewardKind::profileStack;
            prepared.appendedProfileResident = staged.appended && staged.actionSource;
        } else if (bucket.arraySelector == inventory_buckets::ArraySelector::character
                   && detail.instancedDefinitionState
                          == item_details::InstancedDefinitionState::instanced) {
            reason = "instance_quantity";
            if (requested.quantity != 1) {
                return RewardPreparation::unresolvable;
            }
            reason = "instance_capacity";
            PendingItemAcquisition staged{};
            if (!finalize_item_acquisition(
                    working, working, item.definitionHash, false, {.direct = true}, staged)) {
                return RewardPreparation::deferred;
            }
            if (!apply_reward_sockets(
                    detail,
                    requested.sockets,
                    staged.afterCharacter.inventory.values[staged.inventoryIndex].sockets,
                    &reason)) {
                return RewardPreparation::unresolvable;
            }
            working.characters[characterIndex] = staged.afterCharacter;
            prepared.instanceSoid = staged.acquiredInstanceSoid;
            prepared.stateIndex = staged.inventoryIndex;
            prepared.afterQuantity = 1;
            prepared.mutationSerial =
                staged.afterCharacter.inventory.values[staged.inventoryIndex].mutationSerial;
            prepared.inventoryRow = staged.inventoryRow;
            prepared.kind = RecordRewardKind::characterInstance;
        } else if (bucket.arraySelector == inventory_buckets::ArraySelector::character
                   && detail.instancedDefinitionState
                          == item_details::InstancedDefinitionState::stackable
                   && !detail.equipmentSlot.has_value()) {
            reason = "character_stack_capacity";
            CharacterState& character = working.characters[characterIndex];
            if (requested.quantity > detail.maxStackSize) {
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
                    && character.stacks.values[stackIndex].quantity
                           > detail.maxStackSize - requested.quantity)) {
                return RewardPreparation::deferred;
            }
            auto& stack = character.stacks.values[stackIndex];
            if (appended) {
                stack.definitionHash = item.definitionHash;
                ++character.stacks.count;
            }
            stack.quantity += requested.quantity;
            stack.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
            prepared.stateIndex = stackIndex;
            prepared.afterQuantity = stack.quantity;
            prepared.mutationSerial = stack.mutationSerial;
            prepared.kind = RecordRewardKind::characterStack;
        } else {
            return RewardPreparation::unresolvable;
        }
        mutation.rewards[index] = prepared;
    }

    reason = reward_change_refusal(std::span(mutation.rewards).first(rewards.size()));
    if (reason != nullptr) {
        // Only a wrapper draw can exceed a list, and its next draw may fit.
        return RewardPreparation::deferred;
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

} // namespace

/** Prepares every direct reward over one cumulative account view. */
RewardPreparation prepare_record_reward_grant(std::span<const DirectRecordReward> rewards,
                                              std::uint16_t claimedRecordIndex,
                                              PendingRecordRewardGrant& mutation,
                                              const char** refusal) noexcept {
    return prepare_reward_grant(rewards, claimedRecordIndex, nullptr, mutation, refusal);
}

bool preview_record_reward_grant(const PendingRecordRewardGrant& mutation,
                                 AccountState& after) noexcept {
    after = {};
    return materialize_record_reward(account_snapshot(), mutation, after);
}

/** Commits the shared reward after-image and claim together. */
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
            const auto flag = mutation.rewards[i].acquiredFlag;
            if (flag != build_data::rewards::kAbsent) {
                ready = investment::store::write_unlock(
                    investment::store::Bank::accountFlags, flag, unlocks::kFlagSet);
            }
        }
        for (const auto& flag : mutation.wrapperFlags) {
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

namespace {

/**
 * Emote definitions seeded into the collection item's wheel lanes.
 * All four are universal Common emotes, so no class or race can reject a seeded lane.
 */
constexpr std::uint32_t kYesEmoteDefinitionHash = 3184938442U;
constexpr std::uint32_t kNopeEmoteDefinitionHash = 48790291U;
constexpr std::uint32_t kCasualSitEmoteDefinitionHash = 383973261U;
constexpr std::uint32_t kCheerEmoteDefinitionHash = 2834933816U;

/** Lane order is the client's own wheel layout. */
constexpr std::array<std::uint32_t, authored_inventory::kEmoteCollectionSocketLaneCount>
    kEmoteCollectionDefaultPlugHashes{
        kCheerEmoteDefinitionHash,     // lane 0 -- top
        kCasualSitEmoteDefinitionHash, // lane 1 -- bottom
        kYesEmoteDefinitionHash,       // lane 2 -- left
        kNopeEmoteDefinitionHash,      // lane 3 -- right
    };

/**
 * Resolves and cross-checks the "Emotes" collection item's own configured content.
 * @param definition Receives the matching native item-definition row.
 * @return True only when both rows agree, declare no native equipment slot, and carry exactly the
 *         expected 4 ordinary socket lanes.
 */
[[nodiscard]] bool
resolve_emote_collection_definition(build_data::items::Definition& definition) noexcept {
    item_details::Definition detail{};
    return build_data::find_item_definition_hash(authored_inventory::kEmoteCollectionDefinitionHash,
                                                 definition)
           && definition.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && build_data::find_configured_item_detail(definition.definitionIndex, detail)
           && detail.definitionIndex == definition.definitionIndex
           && detail.definitionHash == authored_inventory::kEmoteCollectionDefinitionHash
           && detail.bucketId == definition.bucketId && !detail.equipmentSlot.has_value()
           && detail.ordinarySocketState == item_details::OrdinarySocketState::present
           && detail.ordinarySocketCount == authored_inventory::kEmoteCollectionSocketLaneCount;
}

/** Checks every seeded plug is installed and allowed in its lane before an item can carry it. */
[[nodiscard]] bool default_plugs_valid(std::uint16_t collectionDefinitionIndex) noexcept {
    for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
        build_data::items::Definition plugDefinition{};
        if (!build_data::find_item_definition_hash(kEmoteCollectionDefaultPlugHashes[lane],
                                                   plugDefinition)
            || !build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                   static_cast<std::uint8_t>(lane),
                                                   plugDefinition.definitionIndex)) {
            return false;
        }
    }
    return true;
}

/** Checks an equipped collection item's plugs so a stale set is repaired, not trusted. */
[[nodiscard]] bool socket_state_sound(const authored_inventory::Item& item,
                                      std::uint16_t collectionDefinitionIndex) noexcept {
    if (item.sockets.policy != authored_inventory::SocketPolicy::authored
        || item.sockets.plugCount != authored_inventory::kEmoteCollectionSocketLaneCount) {
        return false;
    }
    for (std::size_t lane = 0; lane < authored_inventory::kEmoteCollectionSocketLaneCount; ++lane) {
        const std::optional<std::uint32_t>& plugHash = item.sockets.plugs[lane];
        build_data::items::Definition plugDefinition{};
        if (!plugHash.has_value()
            || !build_data::find_item_definition_hash(*plugHash, plugDefinition)
            || !build_data::is_socket_plug_allowed(collectionDefinitionIndex,
                                                   static_cast<std::uint8_t>(lane),
                                                   plugDefinition.definitionIndex)) {
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * Equips each character with the "Emotes" collection item in the emote slot.
 * Its content declares no native slot, so every resolver reaches it through
 * resolve_native_equipment_slot. Its 4 lanes have no native default and are seeded here.
 */
EmoteCollectionOutcome ensure_character_emote_collection() noexcept {
    // The wheel occupies the authored emote equipment slot.
    constexpr std::size_t kEmoteCollectionSlot =
        static_cast<std::size_t>(authored_inventory::EquipmentSlot::emote);

    // Nothing can be concluded before these domains publish, so this is a retry, not a verdict.
    if (!build_data::item_definitions_ready() || !build_data::configured_item_details_ready()
        || !build_data::socket_plug_rules_ready()) {
        return EmoteCollectionOutcome::notReady;
    }
    // Published and still unresolved means the build cannot carry the wheel; a retry never helps.
    build_data::items::Definition collectionDefinition{};
    if (!resolve_emote_collection_definition(collectionDefinition)
        || !default_plugs_valid(collectionDefinition.definitionIndex)) {
        return EmoteCollectionOutcome::unsupported;
    }

    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    if (!account::valid(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::notReady;
    }
    bool changed = false;
    bool failed = false;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount && !failed;
         ++characterIndex) {
        CharacterState& character = candidate.characters[characterIndex];
        auto& collectionSlot = character.equipment.slots[kEmoteCollectionSlot];
        const bool present =
            collectionSlot.has_value()
            && collectionSlot->definitionHash == authored_inventory::kEmoteCollectionDefinitionHash;
        if (present && socket_state_sound(*collectionSlot, collectionDefinition.definitionIndex)) {
            continue;
        }
        if (character.nextInventorySerial
            >= static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
            failed = true;
            break;
        }
        // A repair owns the definition, the sockets and the serial. Every other field, the
        // item-state flags above all, belongs to the player and survives.
        authored_inventory::Item granted = present ? *collectionSlot : authored_inventory::Item{};
        if (!present) {
            std::uint64_t instanceSoid = 0;
            if (!next_item_instance_soid(candidate, instanceSoid)) {
                failed = true;
                break;
            }
            granted.instanceSoid = instanceSoid;
            granted.level = 0;
            granted.quantity = 1;
        }
        granted.definitionHash = authored_inventory::kEmoteCollectionDefinitionHash;
        granted.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
        // Replaced whole: lanes past the used prefix must be empty for the block to validate.
        granted.sockets = authored_inventory::Sockets{};
        granted.sockets.policy = authored_inventory::SocketPolicy::authored;
        granted.sockets.plugCount = kEmoteCollectionDefaultPlugHashes.size();
        for (std::size_t lane = 0; lane < kEmoteCollectionDefaultPlugHashes.size(); ++lane) {
            granted.sockets.plugs[lane] = kEmoteCollectionDefaultPlugHashes[lane];
        }
        collectionSlot = granted;
        changed = true;
    }
    if (failed) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    if (!changed) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::ready;
    }
    if (!account::valid(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    if (!investment::store::write_account(candidate)) {
        investment::store::g_mutex.unlock();
        return EmoteCollectionOutcome::failed;
    }
    investment::store::g_mutex.unlock();
    return EmoteCollectionOutcome::ready;
}

} // namespace sunrise::state
