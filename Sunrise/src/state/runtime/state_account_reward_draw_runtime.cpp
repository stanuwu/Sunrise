/**
 * Reward draws: Season pass claims, installed item rewards and opened stored wrappers resolved
 * through the reward graph.
 * A draw becomes inventory rows through the shared record-grant preparation.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "saved_selection_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

using namespace runtime::detail;
namespace authored_inventory = account::inventory;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;

namespace {

/**
 * Turns one resolved draw into inventory rows and consumed-wrapper flags.
 * @param resolved Grants and wrapper flags of one draw.
 * @param price A vendor row's cost entries the grant spends; empty for none.
 * @param openedInstanceSoid Held wrapper the draw opened, removed in the same grant; zero for none.
 * @param nowSeconds Server time in Unix seconds; a granted bounty's lifetime runs from it.
 * @param mutation Receives the prepared grant.
 * @param refusal Receives a static reason when the result is not prepared.
 * @return Unresolvable when the draw outgrew the grant rows.
 */
[[nodiscard]] RewardPreparation
prepare_resolved_reward(const reward_resolution::Result& resolved,
                        std::span<const build_data::vendors::SaleCost> price,
                        std::uint64_t openedInstanceSoid,
                        std::int64_t nowSeconds,
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
    const RewardGrantRequest request{.rewards = std::span(rows).first(resolved.count),
                                     .wrapperFlags = &resolved.wrapperFlags,
                                     .redeemedInstanceSoid = openedInstanceSoid,
                                     .price = price,
                                     .nowSeconds = nowSeconds};
    return prepare_reward_grant(request, mutation, refusal);
}

/** Classifies a catalog refusal: a verdict unless a catalog was cleared meanwhile. */
[[nodiscard]] RewardPreparation catalog_refusal(const char*& reason) noexcept {
    if (catalogs_settled()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    return RewardPreparation::notReady;
}

/** A claim draws against the saved flags; a replay clears the row's own claim flag first. */
enum class PassResolution { claim, replay };

/**
 * Draws one pass row for the selected character and applies its fixed socket overrides.
 * @param seed Draw seed; a replay with the same seed repeats the claim's draw.
 * @param result Receives the draw.
 * @param reason Receives a static reason on failure when not null.
 * @return False when the row is ineligible, its draw is refused, or it does not fit.
 */
[[nodiscard]] bool resolve_pass(const build_data::season_pass::Reward& reward,
                                const AccountState& account,
                                std::uint64_t seed,
                                reward_resolution::Result& result,
                                PassResolution resolution,
                                const char** reason = nullptr) noexcept {
    const auto refuse = [reason](const char* refusal) noexcept {
        if (reason != nullptr) {
            *reason = refusal;
        }
        return false;
    };
    const auto character = selected_character_index(account);
    if (character >= account.characterCount) {
        return refuse("selected_character");
    }
    unlocks::Table flags{};
    if (!investment::store::read_unlocks(flags, static_cast<int>(character))) {
        return refuse("unlock_read");
    }
    build_data::rewards::Item source{};
    if (!build_data::find_reward_item(reward.itemIndex, source)
        || source.definitionHash != reward.itemHash) {
        return refuse("reward_item");
    }
    if (reward.socketCount > reward.sockets.size()
        || reward.conditionCount > reward.condition.size()) {
        return refuse("reward_shape");
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

/**
 * Replays a claim's draw against the current save and compares it with the prepared grant.
 * @return False when any row, flag, quantity or socket block differs from the replay.
 */
[[nodiscard]] bool reward_matches(const build_data::season_pass::Reward& reward,
                                  const PendingSeasonPassReward& mutation) noexcept {
    const auto* grant = &mutation.grant;
    reward_resolution::Result expected{};
    if (!mutation.prepared || mutation.sourceDefinitionHash != reward.itemHash
        || !resolve_pass(
            reward, bound_account_snapshot(), mutation.seed, expected, PassResolution::replay)
        || expected.count != grant->rewardCount
        || expected.wrapperFlags.count() != grant->wrapperFlagCount) {
        return false;
    }
    for (const auto& flag : grant->wrapper_flags()) {
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
        // A dropped or later discarded instance leaves no sockets to compare.
        const auto held = std::span(grant->afterCharacter.inventory.values)
                              .first(grant->afterCharacter.inventory.count);
        const auto installed =
            std::find_if(held.begin(), held.end(), [&](const auto& item) noexcept {
                return prepared.instanceSoid != 0 && item.instanceSoid == prepared.instanceSoid;
            });
        if (planned.socketCount != 0 && installed != held.end()) {
            item_details::Definition detail{};
            authored_inventory::Sockets sockets{};
            if (prepared.kind != RecordRewardKind::characterInstance
                || !build_data::find_configured_item_detail(planned.itemIndex, detail)
                || !apply_reward_sockets(
                    detail, std::span(planned.sockets).first(planned.socketCount), sockets)
                || installed->sockets.policy != sockets.policy
                || installed->sockets.plugCount != sockets.plugCount
                || installed->sockets.plugs != sockets.plugs) {
                return false;
            }
        }
    }
    return true;
}

/**
 * True for wrappers and direct account perks. Both grant through their reward definition and
 * bypass quest initialization.
 */
bool uses_reward_definition(const build_data::items::Definition& item) noexcept {
    build_data::rewards::Item reward{};
    if (!build_data::find_reward_item(item.definitionIndex, reward)) {
        return false;
    }
    return reward.poolIndex != build_data::rewards::kAbsent
           || (item.bucketId == inventory_buckets::kPerkBucketId
               && reward.acquiredFlag != build_data::rewards::kAbsent);
}

} // namespace

/**
 * Prepares an eligible pass row without writing its claim or acquisition flags.
 * @param seed Caller-drawn entropy; the commit replays the same draw from it.
 * @param refusal Receives a static reason on failure.
 * @return False when the row is missing, unearned, claimed, ineligible or does not fit.
 */
bool prepare_season_pass_reward(std::uint16_t rewardIndex,
                                std::uint64_t seed,
                                std::int64_t nowSeconds,
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
    reason = "reward_definitions";
    if (!build_data::reward_definitions_ready()) {
        return false;
    }
    mutation.seed = seed;
    reward_resolution::Result resolved{};
    reason = "reward_condition";
    if (!resolve_pass(reward,
                      bound_account_snapshot(),
                      mutation.seed,
                      resolved,
                      PassResolution::claim,
                      &reason)) {
        return false;
    }
    reason = "reward_placement";
    if (prepare_resolved_reward(resolved, {}, 0, nowSeconds, mutation.grant, &reason)
        != RewardPreparation::prepared) {
        return false;
    }
    reason = nullptr;
    mutation.sourceDefinitionHash = reward.itemHash;
    mutation.rewardIndex = rewardIndex;
    mutation.prepared = true;
    return true;
}

/** Every item the reward path can place takes it; nothing else is grantable. */
bool item_grantable(std::uint16_t itemIndex) noexcept {
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return false;
    }
    if (uses_reward_definition(item)) {
        return true;
    }
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_configured_item_detail(itemIndex, detail)
        || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)) {
        return false;
    }
    const bool profileStack =
        bucket.arraySelector == inventory_buckets::ArraySelector::profile
        && detail.instancedDefinitionState == item_details::InstancedDefinitionState::stackable;
    return profileStack || bucket.arraySelector == inventory_buckets::ArraySelector::character;
}

/** One instance, draw or perk, or up to one full stack of a stackable item. */
std::int32_t item_grant_step(std::uint16_t itemIndex, std::int32_t remaining) noexcept {
    build_data::items::Definition item{};
    item_details::Definition detail{};
    if (remaining <= 1 || !build_data::find_item_definition_index(itemIndex, item)
        || uses_reward_definition(item)
        || !build_data::find_configured_item_detail(itemIndex, detail)
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || detail.maxStackSize <= 0) {
        return (std::min)(remaining, 1);
    }
    return (std::min)(remaining, detail.maxStackSize);
}

/** One step in every row of the item's own bucket and of its overflow bucket. */
std::int32_t item_grant_capacity(std::uint16_t itemIndex) noexcept {
    constexpr std::int32_t kLargestGrant = (std::numeric_limits<std::int32_t>::max)();
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return 0;
    }
    // A wrapper draws on every step, and each draw lands wherever its pool sends it.
    build_data::rewards::Item reward{};
    if (build_data::find_reward_item(itemIndex, reward)
        && reward.poolIndex != build_data::rewards::kAbsent) {
        return kLargestGrant;
    }
    // An account perk is one acquisition flag.
    if (uses_reward_definition(item)) {
        return 1;
    }
    inventory_buckets::Descriptor own{};
    if (!build_data::find_inventory_bucket_descriptor(item.bucketId, own)) {
        return 0;
    }
    std::int64_t rows = own.slotCount;
    inventory_buckets::Descriptor recovery{};
    if (find_recovery_bucket(item, recovery) && recovery.bucketId != own.bucketId) {
        rows += recovery.slotCount;
    }
    const std::int64_t capacity = rows * item_grant_step(itemIndex, kLargestGrant);
    return static_cast<std::int32_t>((std::min)(capacity, std::int64_t{kLargestGrant}));
}

/** A catalog answer is final once the reward and placement catalogs publish, or rewards refuse. */
bool catalogs_settled() noexcept {
    return build_data::item_definitions_ready() && build_data::configured_item_details_ready()
           && build_data::inventory_bucket_descriptors_ready()
           && build_data::socket_entry_lists_ready()
           && (build_data::reward_definitions_ready()
               || build_data::reward_definitions_unsupported());
}

namespace {

/** The saved state one draw runs against. */
using DrawState = SavedSelection<AccountState>;

/**
 * Checks the catalogs a draw reads. A refused reward graph decides every draw; an unpublished one
 * decides nothing yet.
 * @return Prepared when both catalogs are published.
 */
[[nodiscard]] RewardPreparation draw_catalogs(const char*& reason) noexcept {
    reason = "reward_definitions";
    if (build_data::reward_definitions_unsupported()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    if (!build_data::item_definitions_ready() || !build_data::reward_definitions_ready()) {
        return RewardPreparation::notReady;
    }
    return RewardPreparation::prepared;
}

/**
 * Reads the account and the selected character's unlocks a draw runs against. Call under the
 * store lock.
 * @return Prepared when both read; deferred while State has no selected character.
 */
[[nodiscard]] RewardPreparation read_draw_state(DrawState& state, const char*& reason) noexcept {
    const SavedSelectionRead read = read_saved_selection(state);
    reason = "account_read";
    if (read == SavedSelectionRead::accountStorage || read == SavedSelectionRead::accountInvalid) {
        return RewardPreparation::notReady;
    }
    reason = "selected_character";
    if (read == SavedSelectionRead::unselected) {
        return RewardPreparation::deferred;
    }
    reason = "unlock_read";
    return read == SavedSelectionRead::bankStorage ? RewardPreparation::notReady
                                                   : RewardPreparation::prepared;
}

/**
 * Resolves an installed item or wrapper for the selected character, spending a vendor row's price
 * in the same after-image.
 * A draw that leaves nothing to grant or outgrows a bound is final: a new seed is not a retry.
 * @param price A vendor row's cost entries; empty for none.
 * @param seed Caller-drawn entropy for any wrapper draws.
 * @param refusal Receives a static reason on failure.
 * @return Not ready while a catalog is unpublished or a saved read fails; deferred while State
 *         has no selected character, room or price; unresolvable is final.
 */
[[nodiscard]] RewardPreparation
resolve_item_reward(std::uint16_t itemIndex,
                    std::uint32_t quantity,
                    std::span<const build_data::vendors::SaleCost> price,
                    std::uint64_t seed,
                    std::int64_t nowSeconds,
                    PendingRecordRewardGrant& mutation,
                    const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    mutation = {};
    const auto catalogs = draw_catalogs(reason);
    if (catalogs != RewardPreparation::prepared) {
        return catalogs;
    }
    reason = "item_definition";
    const std::lock_guard lock(investment::store::g_mutex);
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return catalog_refusal(reason);
    }
    DrawState state{};
    const auto read = read_draw_state(state, reason);
    if (read != RewardPreparation::prepared) {
        return read;
    }
    reward_resolution::Result resolved{};
    const auto resolution = reward_resolution::resolve(
        {state.banks, state.account.characters[state.slot].characterClass, seed, &reason},
        itemIndex,
        quantity,
        resolved);
    if (resolution != reward_resolution::Resolution::resolved) {
        return catalog_refusal(reason);
    }
    reason = "reward_placement";
    return prepare_resolved_reward(resolved, price, 0, nowSeconds, mutation, &reason);
}

/**
 * Finds the held row a wrapper opening consumes: one unlocked unit in its own bucket.
 * @return Null when the selected character holds no such row.
 */
[[nodiscard]] const authored_inventory::Item* held_wrapper(const CharacterState& character,
                                                           std::uint64_t instanceSoid) noexcept {
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    const auto found = std::find_if(held.begin(), held.end(), [instanceSoid](const auto& item) {
        return item.instanceSoid == instanceSoid;
    });
    if (instanceSoid == 0 || found == held.end() || found->quantity != 1
        || found->placement != authored_inventory::ItemPlacement::authored
        || (found->flags & authored_inventory::kLockedItemFlag) != 0) {
        return nullptr;
    }
    return &*found;
}

} // namespace

/** The held wrapper leaves in the grant that places its draws, so they publish together. */
RewardPreparation prepare_wrapper_opening(std::uint64_t instanceSoid,
                                          std::uint64_t seed,
                                          std::int64_t nowSeconds,
                                          PendingRecordRewardGrant& mutation,
                                          const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    mutation = {};
    const auto catalogs = draw_catalogs(reason);
    if (catalogs != RewardPreparation::prepared) {
        return catalogs;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    DrawState state{};
    const auto read = read_draw_state(state, reason);
    if (read != RewardPreparation::prepared) {
        return read;
    }
    reason = "held_wrapper";
    const auto* source = held_wrapper(state.account.characters[state.slot], instanceSoid);
    if (source == nullptr) {
        return RewardPreparation::deferred;
    }
    reason = "item_definition";
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_hash(source->definitionHash, item)) {
        return catalog_refusal(reason);
    }
    reward_resolution::Result resolved{};
    const auto resolution = reward_resolution::open(
        {state.banks, state.account.characters[state.slot].characterClass, seed, &reason},
        item.definitionIndex,
        resolved);
    if (resolution != reward_resolution::Resolution::resolved) {
        return catalog_refusal(reason);
    }
    reason = "reward_placement";
    return prepare_resolved_reward(resolved, {}, instanceSoid, nowSeconds, mutation, &reason);
}

/** Resolves an item granted without a price. */
RewardPreparation prepare_item_reward(std::uint16_t itemIndex,
                                      std::uint32_t quantity,
                                      std::uint64_t seed,
                                      std::int64_t nowSeconds,
                                      PendingRecordRewardGrant& mutation,
                                      const char** refusal) noexcept {
    return resolve_item_reward(itemIndex, quantity, {}, seed, nowSeconds, mutation, refusal);
}

/**
 * A flagged acquisition, else the lane's policy bits, which the shipped build sets on the perk
 * bucket alone.
 */
bool vendor_reward_item(std::uint16_t itemIndex) noexcept {
    constexpr std::uint8_t kRewardLanePolicy =
        inventory_buckets::kFifo | inventory_buckets::kNoTransferOnEviction;
    build_data::items::Definition item{};
    if (!build_data::find_item_definition_index(itemIndex, item)) {
        return false;
    }
    // An unpublished graph cannot say whether the item is flagged, so the graph path waits for it.
    if (!build_data::reward_definitions_ready() && !build_data::reward_definitions_unsupported()) {
        return true;
    }
    build_data::rewards::Item reward{};
    if (build_data::find_reward_item(itemIndex, reward)
        && reward.acquiredFlag != build_data::rewards::kAbsent) {
        return true;
    }
    inventory_buckets::Descriptor bucket{};
    return build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
           && bucket.arraySelector == inventory_buckets::ArraySelector::character
           && (bucket.policyFlags & kRewardLanePolicy) == kRewardLanePolicy;
}

/** Reads the saved state only for a row that declares conditions. */
bool vendor_row_available(const build_data::vendors::SaleRow& row,
                          bool& available,
                          const char** refusal) noexcept {
    available = false;
    const char* reason = "availability_unreadable";
    const auto fail = [&reason, refusal]() noexcept {
        if (refusal != nullptr) {
            *refusal = reason;
        }
        return false;
    };
    if (row.availabilityUnreadable || row.availabilityCount > row.availability.size()) {
        return fail();
    }
    if (row.availabilityCount == 0) {
        available = true;
        return true;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    DrawState state{};
    if (read_draw_state(state, reason) != RewardPreparation::prepared) {
        return fail();
    }
    // An eligibility check draws nothing, so it takes no seed.
    constexpr std::uint64_t kNoSeed = 0;
    const reward_resolution::Context context{
        state.banks, state.account.characters[state.slot].characterClass, kNoSeed, &reason};
    return reward_resolution::eligible(
               std::span(row.availability).first(row.availabilityCount), context, available)
           || fail();
}

/** Grants one of the row's item, as every vendor grant does. */
RewardPreparation prepare_vendor_reward(std::uint16_t itemIndex,
                                        std::span<const build_data::vendors::SaleCost> price,
                                        std::uint64_t seed,
                                        std::int64_t nowSeconds,
                                        PendingRecordRewardGrant& mutation,
                                        const char** refusal) noexcept {
    return resolve_item_reward(itemIndex, 1, price, seed, nowSeconds, mutation, refusal);
}

/**
 * Revalidates the prepared reward under the lock that commits its inventory and flags.
 * @param mutation Prepared claim, consumed on every exit.
 * @return False when the replayed draw differs or the commit fails; the claim is revoked then.
 */
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

} // namespace sunrise::state
