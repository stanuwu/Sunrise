#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>

#include "../../core/logging/log.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../../state/build_data/runtime.h"
#include "../../state/investment/store_internal.h"
#include "internal.h"

namespace sunrise::server::bap {
namespace {

/** Logged for a saved row whose hash is not in the item table. */
constexpr std::uint16_t kUnresolvedItemIndex = (std::numeric_limits<std::uint16_t>::max)();

/** Outcome of one settlement attempt. */
enum class WorldRewardSettlement : std::uint8_t {
    granted,
    retired,
    /** State refuses the reward until a saved write or selection change. */
    refused,
    /** Not ready, or a transient failure. */
    kept,
};

/** Revision the last pass with no Family-4 peer started under, and what it left refused. */
struct PeerlessPass {
    std::uint64_t revision{};
    /** Newest row id the pass read; rows up to it were refused when awaitsChange is set. */
    std::uint64_t refusedThrough{};
    bool awaitsChange{};
};

/** Used only under the store lock enqueue_world_reward holds across its pass. */
PeerlessPass g_peerlessPass;

/**
 * Grants and retires one earned reward in one transaction, or retires one State rules out.
 * The item resolves through its reward row, which also starts a placed quest item's quest.
 * @param request Saved reward of the selected character.
 * @return Granted, retired as ungrantable, or refused or kept for a later attempt.
 */
WorldRewardSettlement commit_world_reward(const WorldRewardRequest& request) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        report_reward_refusal("world_settle", request.itemDefinitionIndex, "transaction");
        return WorldRewardSettlement::kept;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> grant(
        new (std::nothrow) state::PendingRecordRewardGrant);
    std::uint64_t seed = 0;
    const bool seeded =
        middleware::crypto::random::fill(std::as_writable_bytes(std::span(&seed, 1)));
    const char* reason = seeded ? "reward_allocation" : "random_source";
    auto preparation = state::RewardPreparation::notReady;
    if (seeded && grant) {
        preparation = state::prepare_item_reward(request.itemDefinitionIndex,
                                                 static_cast<std::uint32_t>(request.quantity),
                                                 seed,
                                                 *grant,
                                                 &reason);
    }
    const bool unresolvable = preparation == state::RewardPreparation::unresolvable;
    bool committed = preparation == state::RewardPreparation::prepared;
    if (unresolvable) {
        return retire_world_reward(transaction, request, reason) ? WorldRewardSettlement::retired
                                                                 : WorldRewardSettlement::kept;
    }
    if (committed) {
        reason = "inventory_commit";
        committed = state::commit_record_reward(*grant);
    }
    // The grant and the row's deletion share the savepoint, so a failed deletion undoes the grant.
    if (committed) {
        reason = "queue_commit";
        committed = complete_world_reward(request.id) && transaction.commit();
    }
    if (!committed) {
        report_reward_refusal("world_settle", request.itemDefinitionIndex, reason);
        return preparation == state::RewardPreparation::deferred ? WorldRewardSettlement::refused
                                                                 : WorldRewardSettlement::kept;
    }
    return WorldRewardSettlement::granted;
}

/** Saves the reward under its stable definition hash and earning character. */
bool enqueue_world_reward(std::uint16_t definitionIndex, std::int32_t quantity) noexcept {
    state::build_data::items::Definition definition;
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)) {
        return false;
    }
    // Holding the store lock, nothing but this row's insert moves the revision before the pass.
    const std::lock_guard lock(state::investment::store::g_mutex);
    const std::uint64_t unchanged = state::investment::store::revision();
    if (!state::investment::store::enqueue_reward(definition.definitionHash, quantity)) {
        return false;
    }
    if (!has_active_family4_peer()) {
        settle_world_reward(unchanged);
    }
    return true;
}

} // namespace

/** Logs one refused reward stage with its native index and a specific reason. */
void report_reward_refusal(const char* stage, std::uint16_t index, const char* reason) noexcept {
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=reward stage=%s index=%u result=refused reason=%s",
                      stage,
                      static_cast<unsigned>(index),
                      reason != nullptr ? reason : "unknown");
}

/** Saves one item reward before its pickup presentation is queued. */
bool arm_world_item_acquisition(std::uint16_t itemDefinitionIndex) noexcept {
    return enqueue_world_reward(itemDefinitionIndex, 1);
}

/** Saves a profile material reward before its pickup presentation is queued. */
bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                        std::int32_t quantity) noexcept {
    return quantity > 0 && enqueue_world_reward(itemDefinitionIndex, quantity);
}

/**
 * Restores the selected character's next reward, retiring rows that can never be granted.
 * @param request Receives the reward; cleared when none is ready.
 * @return False when no reward follows afterId or the item table is not published yet.
 */
bool current_world_reward(WorldRewardRequest& request,
                          std::uint64_t afterId,
                          bool* exhausted) noexcept {
    if (exhausted != nullptr) {
        *exhausted = false;
    }
    state::investment::store::PendingReward saved;
    bool interrupted = false;
    bool readFailed = false;
    while (state::investment::store::next_reward(saved, afterId, readFailed)) {
        request = {};
        request.id = saved.id;
        request.definitionHash = saved.definitionHash;
        request.quantity = saved.quantity;
        const bool itemsPublished = state::build_data::item_definitions_ready();
        state::build_data::items::Definition definition;
        const char* verdict = "reward_quantity";
        if (state::build_data::find_item_definition_hash(saved.definitionHash, definition)) {
            request.itemDefinitionIndex = definition.definitionIndex;
            if (request.quantity > 0) {
                return true;
            }
        } else if (!itemsPublished || !state::build_data::item_definitions_ready()) {
            // An unpublished item table proves nothing; retry later.
            interrupted = true;
            break;
        } else {
            request.itemDefinitionIndex = kUnresolvedItemIndex;
            verdict = "item_definition";
        }
        state::investment::store::Transaction transaction;
        if (!transaction.ready()) {
            report_reward_refusal("world_retire", request.itemDefinitionIndex, "transaction");
            interrupted = true;
            break;
        }
        if (!retire_world_reward(transaction, request, verdict)) {
            interrupted = true;
            break;
        }
    }
    if (readFailed) {
        report_reward_refusal("world_read", kUnresolvedItemIndex, "store_read");
        interrupted = true;
    }
    if (exhausted != nullptr) {
        *exhausted = !interrupted;
    }
    request = {};
    return false;
}

/** Acknowledges exactly the reward being granted or retired. */
bool complete_world_reward(std::uint64_t id) noexcept {
    return state::investment::store::complete_reward(id);
}

/**
 * Deletes the row on the caller's open savepoint, commits, and logs why.
 * @param transaction Open savepoint the deletion commits on.
 * @param reason Static label of the verdict that rules the reward out.
 * @return False when the row stays saved.
 */
bool retire_world_reward(state::investment::store::Transaction& transaction,
                         const WorldRewardRequest& request,
                         const char* reason) noexcept {
    const bool retired = complete_world_reward(request.id) && transaction.commit();
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=reward stage=world_retire id=%llu hash=0x%08X index=%u quantity=%d "
                      "result=%s reason=%s",
                      static_cast<unsigned long long>(request.id),
                      static_cast<unsigned>(request.definitionHash),
                      static_cast<unsigned>(request.itemDefinitionIndex),
                      static_cast<int>(request.quantity),
                      retired ? "retired" : "kept",
                      reason != nullptr ? reason : "unknown");
    return retired;
}

/**
 * A reward that must wait stays saved and steps aside, so later rewards still take their turn.
 * A saved row cannot change another row's grant, so while the new row is the only write since
 * the last pass, the rewards that pass refused are not tried again.
 */
void settle_world_reward(std::uint64_t unchanged) noexcept {
    std::uint64_t afterId = 0;
    if (g_peerlessPass.awaitsChange && unchanged == g_peerlessPass.revision) {
        afterId = g_peerlessPass.refusedThrough;
    }
    const std::uint64_t revision = state::investment::store::revision();
    bool awaitsChange = true;
    bool exhausted = false;
    WorldRewardRequest request;
    while (current_world_reward(request, afterId, &exhausted)) {
        const WorldRewardSettlement settlement = commit_world_reward(request);
        if (settlement == WorldRewardSettlement::granted) {
            arm_account_resync_everywhere();
        }
        awaitsChange = awaitsChange && settlement == WorldRewardSettlement::refused;
        if (settlement == WorldRewardSettlement::refused
            || settlement == WorldRewardSettlement::kept) {
            afterId = request.id;
        }
    }
    g_peerlessPass = {revision, afterId, awaitsChange && exhausted};
}

/** Shutdown grants or retires every reward it can; other characters' rewards stay saved. */
void drain_world_rewards() noexcept {
    WorldRewardRequest request;
    std::uint64_t afterId = 0;
    while (current_world_reward(request, afterId, nullptr)) {
        const WorldRewardSettlement settlement = commit_world_reward(request);
        if (settlement == WorldRewardSettlement::refused
            || settlement == WorldRewardSettlement::kept) {
            afterId = request.id;
        }
    }
}

} // namespace sunrise::server::bap
