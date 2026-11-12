#include <Windows.h>

#include <algorithm>
#include <limits>
#include <span>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/activity/destination/definition.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "../internal.h"
#include "../push/activity/activity_keepalive_push.h"
#include "../push/activity/nat_relay_push.h"
#include "../social_feed_route.h"
#include "pursuit_completion_push.h"
#include "queuez_family_repush.h"
#include "queuez_investment_push.h"
#include "queuez_item_grant_push.h"
#include "queuez_state_validation.h"
#include "state/investment/store_internal.h"

namespace sunrise::server::bap::encrypted {

/** @return The peer's retained row overlay, or empty once its presentation hold has passed. */
std::span<const queuez::AcquisitionPresentationRow>
active_acquisition_presentation_rows(const Session& session) noexcept {
    if (GetTickCount64() >= session.acquisitionPresentationUntilTick
        || session.acquisitionPresentationRowCount > session.acquisitionPresentationRows.size()) {
        return {};
    }
    return std::span(session.acquisitionPresentationRows)
        .first(session.acquisitionPresentationRowCount);
}

namespace {

/** Drops the visual XP notification and republishes the account, which already holds the XP. */
void drop_seasonal_experience_presentation(Session& session) noexcept {
    session.pendingSeasonalExperienceAmount = 0;
    session.pendingSeasonalExperienceMutationSerial = 0;
    bap::arm_account_resync_everywhere();
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=season_xp stage=deferred_presentation result=drop");
}

/** @return The picked character, or null when the account is invalid or nothing is picked. */
[[nodiscard]] const state::CharacterState*
selected_character(const state::AccountState& account) noexcept {
    if (!state::account::valid(account)) {
        return nullptr;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            return &account.characters[index];
        }
    }
    return nullptr;
}

/** What one world reward attempt leaves owed. */
enum class WorldRewardAttempt : std::uint8_t {
    /** Granted, and its frame is in the response. */
    published,
    /** Granted without a frame; the armed account resync carries it. */
    unpublished,
    /** State refused or retired the reward; another attempt waits for the gate to move. */
    refused,
    /** Not ready, or a transient failure; the next poll tries again. */
    retry,
};

/** Grants and publishes one world reward, retiring one that State can never grant. */
[[nodiscard]] WorldRewardAttempt consume_world_reward(const WorldRewardRequest& request,
                                                      Session& session,
                                                      Scratch& scratch,
                                                      std::span<std::byte> response,
                                                      std::size_t& written,
                                                      bool& touchesScratch) noexcept {
    const auto fail = [&](const char* reason, WorldRewardAttempt attempt) noexcept {
        if (reason != nullptr) {
            report_reward_refusal("world_publish", request.itemDefinitionIndex, reason);
        }
        return attempt;
    };
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return fail("transaction", WorldRewardAttempt::retry);
    }
    StagedItemGrant staged{};
    const char* reason = nullptr;
    switch (stage_item_grant({request.itemDefinitionIndex, request.quantity},
                             session,
                             scratch,
                             response,
                             touchesScratch,
                             staged,
                             reason)) {
    case ItemGrantStage::committed:
        break;
    case ItemGrantStage::unresolvable:
        return bap::retire_world_reward(transaction, request, reason) ? WorldRewardAttempt::refused
                                                                      : WorldRewardAttempt::retry;
    case ItemGrantStage::deferred:
        return fail(reason, WorldRewardAttempt::refused);
    case ItemGrantStage::waiting:
    case ItemGrantStage::notReady:
    case ItemGrantStage::failed:
        return fail(reason, WorldRewardAttempt::retry);
    }
    if (!bap::complete_world_reward(request.id) || !transaction.commit()) {
        return fail("queue_commit", WorldRewardAttempt::retry);
    }
    if (!publish_item_grant(staged, session, scratch, response, written)) {
        return fail(staged.unpublished, WorldRewardAttempt::unpublished);
    }
    return WorldRewardAttempt::published;
}

/**
 * Publishes the oldest saved world reward that can go out on this poll. A reward State refuses
 * steps aside for later ones, and once State refuses every saved reward, polls skip the queue
 * until saved rows, the selected character or catalog readiness change.
 */
[[nodiscard]] bool consume_world_rewards(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept {
    // A draw is not retained, so rewards wait for a buffer any frame is sure to fit.
    if (!session.queuez.family4Active || response.size() < scratch.framed.size()) {
        return false;
    }
    const WorldRewardGate gate{state::investment::store::revision(), state::catalogs_settled()};
    if (session.worldRewardsAwaitChange && gate == session.worldRewardGate) {
        return false;
    }
    bool awaitsChange = true;
    bool exhausted = false;
    std::uint64_t afterId = 0;
    WorldRewardRequest reward{};
    while (bap::current_world_reward(reward, afterId, &exhausted)) {
        // Every saved kind is placed by the same grant path, overflow included.
        const WorldRewardAttempt attempt =
            consume_world_reward(reward, session, scratch, response, written, touchesScratch);
        if (attempt == WorldRewardAttempt::published) {
            return true;
        }
        if (attempt == WorldRewardAttempt::unpublished || session.accountResyncArmed) {
            return false;
        }
        // Catalogs that settle move the gate, so a reward waiting on them need not retry per poll.
        awaitsChange =
            awaitsChange && (attempt == WorldRewardAttempt::refused || !gate.catalogsSettled);
        afterId = reward.id;
    }
    session.worldRewardsAwaitChange = awaitsChange && exhausted;
    session.worldRewardGate = gate;
    return false;
}

/** Publishes one non-persistent XP reward row so the native seasonal XP HUD animates. */
[[nodiscard]] bool consume_seasonal_experience_presentation(Session& session,
                                                            Scratch& scratch,
                                                            std::span<std::byte> response,
                                                            std::size_t& written,
                                                            bool& touchesScratch) noexcept {
    if (session.pendingSeasonalExperienceAmount <= 0) {
        return false;
    }
    if (session.pendingSeasonalExperienceMutationSerial == 0) {
        std::int32_t mutationSerial = 0;
        // The row belongs to the picked character, so the gain waits for a pick.
        if (!state::reserve_selected_character_inventory_serial(mutationSerial)) {
            return false;
        }
        session.pendingSeasonalExperienceMutationSerial =
            static_cast<std::uint32_t>(mutationSerial) + 1U;
    }
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    if (!push::append_seasonal_experience_notification(
            scratch,
            session.queuez,
            session.pendingSeasonalExperienceAmount,
            static_cast<std::int32_t>(session.pendingSeasonalExperienceMutationSerial - 1U),
            active_acquisition_presentation_rows(session),
            session.sessionKey,
            nextSendNonce,
            scratch.framed,
            framedSize,
            after)
        || framedSize == 0 || framedSize > response.size()) {
        drop_seasonal_experience_presentation(session);
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    session.pendingSeasonalExperienceAmount = 0;
    session.pendingSeasonalExperienceMutationSerial = 0;
    bap::arm_account_resync_elsewhere(session);
    return true;
}

/** Publishes the current account graph to a peer invalidated by another connection. */
[[nodiscard]] bool consume_account_resync(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    if (!session.accountResyncArmed) {
        return false;
    }
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState currentQueuez{};
    if (!push::append_account_resync_notification(scratch,
                                                  session.queuez,
                                                  active_acquisition_presentation_rows(session),
                                                  session.sessionKey,
                                                  nextSendNonce,
                                                  scratch.framed,
                                                  framedSize,
                                                  currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=family4");
        return false;
    }
    bool auxiliaryRefreshFailed = false;
    if (currentQueuez.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (!push::append_account_resync_appearance_notification(scratch,
                                                                 currentQueuez,
                                                                 session.sessionKey,
                                                                 nextSendNonce,
                                                                 scratch.framed,
                                                                 framedSize,
                                                                 appearanceAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family0");
            auxiliaryRefreshFailed = true;
        } else {
            currentQueuez = appearanceAfter;
        }
    }
    if (currentQueuez.family3Active) {
        queuez::SessionState rosterAfter{};
        if (!push::append_account_resync_roster_notification(scratch,
                                                             currentQueuez,
                                                             session.sessionKey,
                                                             nextSendNonce,
                                                             scratch.framed,
                                                             framedSize,
                                                             rosterAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family3");
            auxiliaryRefreshFailed = true;
        } else {
            currentQueuez = rosterAfter;
        }
    }
    if (framedSize == 0 || framedSize > response.size() || !queuez::valid(currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=output");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = currentQueuez;
    session.accountResyncArmed = false;
    // The account image just sent holds every queued item's current values.
    session.itemValuesRefreshCount = 0;
    if (auxiliaryRefreshFailed) {
        // Family 4 already produced a complete frame. Appearance and roster are derived views,
        // so they retry in their own deferred lane rather than holding the account update.
        session.abilityRefreshDueTick = GetTickCount64();
        session.characterRefreshScope = CharacterRefreshScope::recordsAndRoster;
    }
    return true;
}

/** Removes the oldest queued item-values refresh. */
void retire_item_values_refresh(Session& session) noexcept {
    const auto queued =
        std::span(session.itemValuesRefreshSoids).first(session.itemValuesRefreshCount);
    std::shift_left(queued.begin(), queued.end(), 1);
    --session.itemValuesRefreshCount;
}

/** @return True while the character holds the unequipped instance. */
[[nodiscard]] bool holds_item(const state::CharacterState& character,
                              std::uint64_t instanceSoid) noexcept {
    const auto held = std::span(character.inventory.values).first(character.inventory.count);
    return std::any_of(held.begin(), held.end(), [instanceSoid](const auto& item) noexcept {
        return item.instanceSoid == instanceSoid;
    });
}

/**
 * Publishes the oldest queued item's resident with its character, built from the saved account as
 * it stands. An item the selected character no longer holds is retired unsent. A refresh that
 * cannot be staged or encoded arms this peer's account resync, which carries every queued item.
 * @param session Auth, nonce, queuez state and the queue owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when one item's refresh is published.
 */
[[nodiscard]] bool consume_item_values_refresh(Session& session,
                                               Scratch& scratch,
                                               std::span<std::byte> response,
                                               std::size_t& written,
                                               bool& touchesScratch) noexcept {
    if (session.itemValuesRefreshCount == 0) {
        return false;
    }
    if (!session.queuez.family4Active) {
        // A peer that left Family 4 reads the whole account again when it subscribes.
        session.itemValuesRefreshCount = 0;
        return false;
    }
    // The frame is not kept between polls, so it waits for a buffer any frame is sure to fit.
    if (response.size() < scratch.framed.size()) {
        return false;
    }
    const state::AccountState account = state::bound_account_snapshot();
    // The frame carries the picked character's object; with none picked the resync carries it.
    const state::CharacterState* selected = selected_character(account);
    if (selected == nullptr) {
        session.itemValuesRefreshCount = 0;
        session.accountResyncArmed = true;
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=item_values_refresh result=fail reason=selection");
        return false;
    }
    // A bounty redeemed or discarded since it advanced has nothing left to show.
    while (session.itemValuesRefreshCount != 0
           && !holds_item(*selected, session.itemValuesRefreshSoids.front())) {
        retire_item_values_refresh(session);
    }
    if (session.itemValuesRefreshCount == 0) {
        return false;
    }
    const std::uint64_t instanceSoid = session.itemValuesRefreshSoids.front();
    const state::AccountEditChange change{
        .accountSoid = account.primarySoid,
        .characterSoid = selected->soid,
        .characterIndex = static_cast<std::size_t>(selected - account.characters.data()),
        .itemInstanceSoid = instanceSoid};
    touchesScratch = true;
    StagedFrame frame{};
    const char* reason = nullptr;
    if (!stage_changed_objects(change, session, scratch, frame, reason)) {
        session.accountResyncArmed = true;
        core::log::writef(core::log::Channel::server,
                          core::log::Level::warn,
                          "ev=queuez stage=item_values_refresh result=fail reason=%s "
                          "instance=0x%llX",
                          reason,
                          static_cast<unsigned long long>(instanceSoid));
        return false;
    }
    std::copy_n(scratch.framed.begin(), frame.size, response.begin());
    written = frame.size;
    session.sendNonce = frame.sendNonce;
    session.queuez = frame.after;
    retire_item_values_refresh(session);
    return true;
}

/**
 * Refreshes character records after an asynchronous ability-bucket rebuild.
 * @param session Auth, nonce, queuez state and the refresh arm owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a refresh frame is published. With no pick, the arm clears and none is.
 */
[[nodiscard]] bool consume_ability_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    if (session.characterRefreshScope == CharacterRefreshScope::none
        || GetTickCount64() < session.abilityRefreshDueTick) {
        return false;
    }
    // Retain the arm until a family that reads abilities is active.
    if (!session.queuez.family0Active && !session.queuez.family3Active) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState current = session.queuez;
    if (current.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (!push::append_account_resync_appearance_notification(scratch,
                                                                 current,
                                                                 session.sessionKey,
                                                                 nextSendNonce,
                                                                 scratch.framed,
                                                                 framedSize,
                                                                 appearanceAfter)) {
            return false;
        }
        current = appearanceAfter;
    }
    if (current.family3Active) {
        queuez::SessionState rosterAfter{};
        if (!push::append_account_resync_roster_notification(
                scratch,
                current,
                session.sessionKey,
                nextSendNonce,
                scratch.framed,
                framedSize,
                rosterAfter,
                session.characterRefreshScope == CharacterRefreshScope::recordsAndRoster)) {
            return false;
        }
        current = rosterAfter;
    }
    if (framedSize == 0 && queuez::valid(current)) {
        // Both lanes appended nothing because nothing is picked, so no refresh is owed.
        session.characterRefreshScope = CharacterRefreshScope::none;
        return false;
    }
    if (framedSize == 0 || framedSize > response.size() || !queuez::valid(current)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=ability_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = current;
    // Clear the arm only after publication.
    session.characterRefreshScope = CharacterRefreshScope::none;
    return true;
}

/**
 * Publishes the account unlock overrides an artifact purchase or reset changed.
 * The arm is spent before the frame is built, so one committed change owes exactly one frame.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when the family-five snapshot is published.
 */
[[nodiscard]] bool consume_artifact_family5_refresh(Session& session,
                                                    Scratch& scratch,
                                                    std::span<std::byte> response,
                                                    std::size_t& written,
                                                    bool& touchesScratch) noexcept {
    if (!session.artifactRefreshArmed) {
        return false;
    }
    session.artifactRefreshArmed = false;
    if (session.queuez.family5Version == (std::numeric_limits<std::int32_t>::max)()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=family5_refresh result=fail reason=version");
        return false;
    }
    touchesScratch = true;
    const std::int32_t version = session.queuez.family5Version + 1;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_family5_override_notification(
            scratch, version, session.sessionKey, nextSendNonce, scratch.framed, framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=family5_refresh result=fail reason=frame");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez.family5Version = version;
    // The client rebuilds its derived unlock state on the family-4 update that follows this.
    return true;
}

/** Re-publishes only the selected character after an artifact purchase. */
[[nodiscard]] bool consume_artifact_family4_refresh(Session& session,
                                                    Scratch& scratch,
                                                    std::span<std::byte> response,
                                                    std::size_t& written,
                                                    bool& touchesScratch) noexcept {
    if (!session.artifactFamily4RefreshArmed
        || GetTickCount64() < session.artifactFamily4RefreshDueTick) {
        return false;
    }
    const state::AccountState account = state::bound_account_snapshot();
    const state::CharacterState* selected = selected_character(account);
    if (selected == nullptr) {
        return false;
    }
    state::PendingArtifactPurchase refresh{};
    refresh.accountSoid = account.primarySoid;
    refresh.characterSoid = selected->soid;
    refresh.characterIndex = static_cast<std::size_t>(selected - account.characters.data());
    refresh.beforeMask = state::artifact_mod_mask();
    refresh.afterMask = refresh.beforeMask;
    refresh.prepared = true;

    queuez::EquipmentSwap update{};
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    touchesScratch = true;
    const auto presentationRows = active_acquisition_presentation_rows(session);
    if (!queuez::stage_equipment_swap(session.queuez, refresh.characterSoid, update)
        || !push::append_artifact_purchase_notification(scratch,
                                                        update,
                                                        refresh,
                                                        presentationRows,
                                                        session.sessionKey,
                                                        nextSendNonce,
                                                        scratch.framed,
                                                        framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=artifact_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = update.after;
    session.artifactFamily4RefreshArmed = false;
    session.artifactFamily4RefreshDueTick = 0;
    return true;
}

/** Publishes one reset-affected item resident per poll using the proven socket-update shape. */
[[nodiscard]] bool consume_artifact_item_refresh(Session& session,
                                                 Scratch& scratch,
                                                 std::span<std::byte> response,
                                                 std::size_t& written,
                                                 bool& touchesScratch) noexcept {
    if (session.artifactResetRefreshCursor >= session.artifactResetRefresh.instanceCount) {
        session.artifactResetRefresh = {};
        session.artifactResetRefreshCursor = 0;
        return false;
    }
    const state::AccountState account = state::bound_account_snapshot();
    const state::CharacterState* selected = selected_character(account);
    if (selected == nullptr) {
        return false;
    }
    const std::uint64_t instanceSoid =
        session.artifactResetRefresh.instanceSoids[session.artifactResetRefreshCursor];
    queuez::EquipmentSwap update{};
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    touchesScratch = true;
    if (!queuez::stage_equipment_swap(session.queuez, selected->soid, update)
        || !push::append_artifact_item_refresh_notification(scratch,
                                                            update,
                                                            instanceSoid,
                                                            session.sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=artifact_item_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    middleware::secure_channel::advance_nonce(nextSendNonce);
    session.sendNonce = nextSendNonce;
    session.queuez = update.after;
    ++session.artifactResetRefreshCursor;
    if (session.artifactResetRefreshCursor >= session.artifactResetRefresh.instanceCount) {
        // Equipped sockets feed Family 0/3's derived perk banks. Refresh them once every
        // changed item resident has landed so reset cannot leave the previous champion effect
        // cached.
        session.abilityRefreshDueTick = GetTickCount64();
        if (session.characterRefreshScope == CharacterRefreshScope::none) {
            session.characterRefreshScope = CharacterRefreshScope::records;
        }
    }
    return true;
}

} // namespace

/** Publishes the next due reward, refresh, retry, or keepalive. */
bool consume_deferred(Session& session,
                      Scratch& scratch,
                      std::span<std::byte> response,
                      std::size_t& written,
                      bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated) {
        return false;
    }
    if (push::activity::consume_relay_notifications(
            session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // Ahead of the queuez poll so a family burst cannot starve a guest's social publication.
    if (consume_social_notice(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_reward_mail(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (public_queuez::poll(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // The overrides go first: they are what the purchased mod unlocks, and the Family-4
    // companion waits on its own delay.
    if (consume_artifact_family5_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_artifact_family4_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_artifact_item_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_account_resync(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // A failed resync blocks every incremental that could depend on its missing objects.
    if (session.accountResyncArmed) {
        return false;
    }
    // Queued item values are already saved, so they go out before a lane commits anything newer.
    if (consume_item_values_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // A refresh that could not be built armed the resync, which must be sent before any increment.
    // One still waiting for a whole buffer goes out before anything newer.
    if (session.accountResyncArmed || session.itemValuesRefreshCount != 0) {
        return false;
    }
    if (consume_pursuit_completion(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (consume_world_rewards(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (session.accountResyncArmed) {
        return false;
    }
    if (consume_seasonal_experience_presentation(
            session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // Editor operations wait for any queued XP toast, so no stale gain shows after an XP edit.
    if (consume_investment_operation(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // A grant committed without a frame armed a resync, which must be sent before any increment.
    if (session.accountResyncArmed) {
        return false;
    }
    if (consume_ability_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (!session.family4RepushArmed || session.family4RepushRoot == 0
        || GetTickCount64() < session.family4RepushDueTick
        || GetTickCount64() < session.acquisitionPresentationUntilTick) {
        return consume_social_roster_repush(session, scratch, response, written, touchesScratch)
               || consume_banner_repush(session, scratch, response, written, touchesScratch)
               || push::activity::consume_activity_keepalive(
                   session, scratch, response, written, touchesScratch);
    }
    return consume_family4_repush(session, scratch, response, written, touchesScratch);
}

} // namespace sunrise::server::bap::encrypted
