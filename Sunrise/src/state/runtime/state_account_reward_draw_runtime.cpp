/**
 * Reward draws: Season pass claims and installed item rewards resolved through the reward graph.
 * A draw becomes inventory rows through the shared record-grant preparation.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../rewards/reward_resolver.h"
#include "runtime.h"
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
 * @param mutation Receives the prepared grant.
 * @param refusal Receives a static reason when the result is not prepared.
 * @return Unresolvable when the draw outgrew the grant rows.
 */
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

/** @return True when a catalog answer is final: both catalogs published, or rewards refused. */
[[nodiscard]] bool catalogs_settled() noexcept {
    return build_data::item_definitions_ready()
           && (build_data::reward_definitions_ready()
               || build_data::reward_definitions_unsupported());
}

/** Classifies a catalog refusal: a verdict unless a catalog was cleared meanwhile. */
[[nodiscard]] RewardPreparation catalog_refusal(const char*& reason) noexcept {
    if (catalogs_settled()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
    return RewardPreparation::deferred;
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
            reward, account_snapshot(), mutation.seed, expected, PassResolution::replay)
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

} // namespace

/**
 * Prepares an eligible pass row without writing its claim or acquisition flags.
 * @param seed Caller-drawn entropy; the commit replays the same draw from it.
 * @param refusal Receives a static reason on failure.
 * @return False when the row is missing, unearned, claimed, ineligible or does not fit.
 */
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
    reason = "reward_definitions";
    if (!build_data::reward_definitions_ready()) {
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

/**
 * Resolves an installed item or wrapper for the selected character.
 * A draw that leaves nothing to grant or outgrows a bound is final: a new seed is not a retry.
 * @param seed Caller-drawn entropy for any wrapper draws.
 * @param refusal Receives a static reason on failure.
 * @return Deferred while a catalog, the selected character or inventory room is missing;
 *         unresolvable is final.
 */
RewardPreparation prepare_item_reward(std::uint16_t itemIndex,
                                      std::uint32_t quantity,
                                      std::uint64_t seed,
                                      PendingRecordRewardGrant& mutation,
                                      const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    mutation = {};
    // A refused reward graph decides every grant; an unpublished one decides nothing yet.
    reason = "reward_definitions";
    if (build_data::reward_definitions_unsupported()) {
        return RewardPreparation::unresolvable;
    }
    reason = "not_ready";
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
    // A whole account is large, so its snapshot is owned on the heap like the claim path's.
    const std::unique_ptr<const AccountState> snapshot(new (std::nothrow)
                                                           AccountState(account_snapshot()));
    if (!snapshot) {
        return RewardPreparation::deferred;
    }
    const AccountState& account = *snapshot;
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
    if (resolution != reward_resolution::Resolution::resolved) {
        return catalog_refusal(reason);
    }
    reason = "reward_placement";
    return prepare_resolved_reward(resolved, mutation, &reason);
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
