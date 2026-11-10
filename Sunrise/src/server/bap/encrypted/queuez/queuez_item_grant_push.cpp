#include "queuez_item_grant_push.h"

#include <algorithm>
#include <memory>
#include <new>
#include <span>

#include "../../../../core/runtime/server_clock.h"
#include "../../../../middleware/crypto/random_bytes.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {

/** Draws and places the item through its installed reward definition. */
ItemGrantStage stage_item_grant(const ItemGrant& grant,
                                Session& session,
                                Scratch& scratch,
                                std::span<std::byte> response,
                                bool& touchesScratch,
                                StagedItemGrant& staged,
                                const char*& reason) noexcept {
    staged = {};
    reason = nullptr;
    // A draw is not kept between polls, so wait for a response that can hold a whole frame.
    if (response.size() < scratch.framed.size()) {
        return ItemGrantStage::waiting;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> pending(
        new (std::nothrow) state::PendingRecordRewardGrant);
    if (!pending) {
        reason = "reward_allocation";
        return ItemGrantStage::failed;
    }
    std::uint64_t seed = 0;
    if (!middleware::crypto::random::draw_seed(seed)) {
        reason = "random_source";
        return ItemGrantStage::failed;
    }
    const auto preparation = state::prepare_item_reward(grant.itemDefinitionIndex,
                                                        static_cast<std::uint32_t>(grant.quantity),
                                                        seed,
                                                        core::runtime::server_clock_seconds(),
                                                        *pending,
                                                        &reason);
    if (preparation == state::RewardPreparation::unresolvable) {
        return ItemGrantStage::unresolvable;
    }
    if (preparation == state::RewardPreparation::notReady) {
        return ItemGrantStage::notReady;
    }
    if (preparation != state::RewardPreparation::prepared) {
        return ItemGrantStage::deferred;
    }
    touchesScratch = true;
    queuez::RecordRewardGrant update{};
    staged.frame.sendNonce = session.sendNonce;
    const bool stagedUpdate = queuez::stage_record_reward_grant(session.queuez, *pending, update);
    const bool encoded =
        stagedUpdate
        && push::append_record_reward_notification(scratch,
                                                   session.queuez,
                                                   update,
                                                   *pending,
                                                   active_acquisition_presentation_rows(session),
                                                   session.sessionKey,
                                                   staged.frame.sendNonce,
                                                   scratch.framed,
                                                   staged.frame.size)
        && staged.frame.size != 0;
    const auto rewards = std::span(pending->rewards).first(pending->rewardCount);
    staged.placed = std::any_of(rewards.begin(), rewards.end(), [](const auto& reward) noexcept {
        return reward.placedQuantity != 0;
    });
    if (!state::commit_record_reward(*pending)) {
        reason = "inventory_commit";
        return ItemGrantStage::failed;
    }
    // The staged nonce steps past the frame. A committed grant without one records why.
    if (encoded) {
        middleware::secure_channel::advance_nonce(staged.frame.sendNonce);
        staged.frame.after = update.after;
    } else {
        staged.unpublished = stagedUpdate ? "inventory_encode" : "inventory_stage";
    }
    return ItemGrantStage::committed;
}

/** Sends the frame, moves the session past it and publishes the committed account mutation. */
void publish_staged_frame(const StagedFrame& frame,
                          Session& session,
                          Scratch& scratch,
                          std::span<std::byte> response,
                          std::size_t& written) noexcept {
    std::copy_n(scratch.framed.begin(), frame.size, response.begin());
    written = frame.size;
    session.sendNonce = frame.sendNonce;
    session.queuez = frame.after;
    bap::publish_account_mutation(session);
}

/** A committed grant without a frame cannot be retried, so every Family-4 peer resyncs. */
bool publish_item_grant(const StagedItemGrant& staged,
                        Session& session,
                        Scratch& scratch,
                        std::span<std::byte> response,
                        std::size_t& written) noexcept {
    if (staged.unpublished != nullptr) {
        bap::arm_account_resync_everywhere();
        return false;
    }
    publish_staged_frame(staged.frame, session, scratch, response, written);
    bap::arm_acquisition_presentation_hold(session);
    if (!session.artifactRefreshArmed) {
        // The grant published the account; the client rebuilds what its sockets offer on the
        // character copy that follows. An armed Family-5 refresh already brings one.
        bap::arm_unlock_character_refresh(session);
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted
