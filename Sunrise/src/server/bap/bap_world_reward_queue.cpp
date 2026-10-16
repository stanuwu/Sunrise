#include <limits>
#include <memory>
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
enum class WorldRewardSettlement : std::uint8_t { granted, retired, kept };

/** Grants and retires one earned reward in one transaction, or retires one State rules out. */
WorldRewardSettlement commit_world_reward(const WorldRewardRequest& request) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return WorldRewardSettlement::kept;
    }
    bool committed = false;
    bool unresolvable = false;
    const char* reason = nullptr;
    if (request.kind == WorldRewardKind::item) {
        if (state::item_grant_route(request.itemDefinitionIndex) == state::ItemGrantRoute::quest) {
            if (request.quantity != 1) {
                unresolvable = true;
                reason = "instance_quantity";
            } else {
                state::PendingItemAcquisition acquisition;
                committed = state::prepare_item_acquisition_for_item(request.itemDefinitionIndex,
                                                                     acquisition)
                            && state::commit_item_acquisition(acquisition);
            }
        } else {
            const std::unique_ptr<state::PendingRecordRewardGrant> grant(
                new (std::nothrow) state::PendingRecordRewardGrant);
            std::uint64_t seed = 0;
            const bool seeded =
                middleware::crypto::random::fill(std::as_writable_bytes(std::span(&seed, 1)));
            reason = seeded ? "reward_allocation" : "random_source";
            auto preparation = state::RewardPreparation::deferred;
            if (seeded && grant) {
                preparation =
                    state::prepare_item_reward(request.itemDefinitionIndex,
                                               static_cast<std::uint32_t>(request.quantity),
                                               seed,
                                               *grant,
                                               &reason);
            }
            unresolvable = preparation == state::RewardPreparation::unresolvable;
            committed = preparation == state::RewardPreparation::prepared;
            if (committed) {
                reason = "inventory_commit";
                committed = state::commit_record_reward(*grant);
            }
            if (!committed && !unresolvable) {
                report_reward_refusal("world_settle", request.itemDefinitionIndex, reason);
            }
        }
    } else {
        state::PendingProfileItemAcquisition acquisition;
        committed = state::prepare_profile_item_acquisition_for_item(
                        request.itemDefinitionIndex, request.quantity, acquisition)
                    && state::commit_profile_item_acquisition(acquisition);
    }
    if (unresolvable) {
        return retire_world_reward(transaction, request, reason) ? WorldRewardSettlement::retired
                                                                 : WorldRewardSettlement::kept;
    }
    if (!committed || !complete_world_reward(request.id) || !transaction.commit()) {
        return WorldRewardSettlement::kept;
    }
    return WorldRewardSettlement::granted;
}

/** Saves the reward under its stable definition hash and earning character. */
bool enqueue_world_reward(std::uint16_t definitionIndex,
                          std::int32_t quantity,
                          WorldRewardKind kind) noexcept {
    state::build_data::items::Definition definition;
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)
        || !state::investment::store::enqueue_reward(
            definition.definitionHash, quantity, static_cast<std::uint8_t>(kind))) {
        return false;
    }
    if (!has_active_family4_peer()) {
        settle_world_reward();
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
    return enqueue_world_reward(itemDefinitionIndex, 1, WorldRewardKind::item);
}

/** Saves a profile material reward before its pickup presentation is queued. */
bool arm_world_profile_item_acquisition(std::uint16_t itemDefinitionIndex,
                                        std::int32_t quantity) noexcept {
    return quantity > 0
           && enqueue_world_reward(itemDefinitionIndex, quantity, WorldRewardKind::profileItem);
}

/** Restores the selected character's oldest reward, retiring rows that can never be granted. */
bool current_world_reward(WorldRewardRequest& request) noexcept {
    state::investment::store::PendingReward saved;
    while (state::investment::store::next_reward(saved)) {
        request = {};
        request.id = saved.id;
        request.definitionHash = saved.definitionHash;
        request.quantity = saved.quantity;
        request.kind = static_cast<WorldRewardKind>(saved.kind);
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
            break;
        } else {
            request.itemDefinitionIndex = kUnresolvedItemIndex;
            verdict = "item_definition";
        }
        state::investment::store::Transaction transaction;
        if (!transaction.ready() || !retire_world_reward(transaction, request, verdict)) {
            break;
        }
    }
    request = {};
    return false;
}

/** Acknowledges exactly the reward being granted or retired. */
bool complete_world_reward(std::uint64_t id) noexcept {
    return state::investment::store::complete_reward(id);
}

/** Deletes the row on the caller's open savepoint, commits, and logs why. */
bool retire_world_reward(state::investment::store::Transaction& transaction,
                         const WorldRewardRequest& request,
                         const char* reason) noexcept {
    const bool retired = complete_world_reward(request.id) && transaction.commit();
    core::log::writef(core::log::Channel::server,
                      core::log::Level::warn,
                      "ev=reward stage=world_retire id=%llu hash=0x%08X index=%u quantity=%d "
                      "kind=%u result=%s reason=%s",
                      static_cast<unsigned long long>(request.id),
                      static_cast<unsigned>(request.definitionHash),
                      static_cast<unsigned>(request.itemDefinitionIndex),
                      static_cast<int>(request.quantity),
                      static_cast<unsigned>(request.kind),
                      retired ? "retired" : "kept",
                      reason != nullptr ? reason : "unknown");
    return retired;
}

/** A retired reward lets the next one take its turn; any other failed grant stays saved. */
void settle_world_reward() noexcept {
    WorldRewardRequest request;
    while (current_world_reward(request)) {
        const WorldRewardSettlement settlement = commit_world_reward(request);
        if (settlement == WorldRewardSettlement::granted) {
            arm_account_resync_everywhere();
        }
        if (settlement != WorldRewardSettlement::retired) {
            return;
        }
    }
}

/** Shutdown grants or retires rewards until one must wait; other characters' rewards stay saved. */
void drain_world_rewards() noexcept {
    WorldRewardRequest request;
    while (current_world_reward(request)) {
        if (commit_world_reward(request) == WorldRewardSettlement::kept) {
            break;
        }
    }
}

} // namespace sunrise::server::bap
