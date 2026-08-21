#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"
#include "../internal.h"
#include "../push/activity/activity_keepalive_push.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Widest re-push report, sized for the fields below. */
constexpr std::size_t kRepushReportLimit = 96;

/**
 * Logs one delayed re-push with its framed size, so it can be compared to the first copy.
 * @param stage Point in the deferred push the line reports.
 * @param bytes Framed size of the published notification.
 */
void report_repush(const char* stage, std::size_t bytes) noexcept {
    std::array<char, kRepushReportLimit> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=queuez stage=%s result=ok bytes=%zu", stage, bytes);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Publishes the current account graph to a peer invalidated by another connection. */
[[nodiscard]] bool consume_account_resync(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    if (!session.accountResyncArmed || session.accountResyncGeneration == 0) {
        return false;
    }
    touchesScratch = true;
    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState currentQueuez{};
    if (!push::append_account_resync_notification(scratch,
                                                  session.queuez,
                                                  state::bap().sessionKey,
                                                  nextSendNonce,
                                                  scratch.framed,
                                                  framedSize,
                                                  currentQueuez)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=peer_resync result=fail reason=family4");
        return false;
    }
    if (currentQueuez.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (!push::append_account_resync_appearance_notification(scratch,
                                                                 currentQueuez,
                                                                 state::bap().sessionKey,
                                                                 nextSendNonce,
                                                                 scratch.framed,
                                                                 framedSize,
                                                                 appearanceAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family0");
            return false;
        }
        currentQueuez = appearanceAfter;
    }
    if (currentQueuez.family3Active) {
        queuez::SessionState rosterAfter{};
        if (!push::append_account_resync_roster_notification(scratch,
                                                             currentQueuez,
                                                             state::bap().sessionKey,
                                                             nextSendNonce,
                                                             scratch.framed,
                                                             framedSize,
                                                             rosterAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=peer_resync result=fail reason=family3");
            return false;
        }
        currentQueuez = rosterAfter;
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
    session.accountGeneration = session.accountResyncGeneration;
    session.accountResyncArmed = false;
    report_repush("peer_resync", framedSize);
    return true;
}

/**
 * Sends the owed banner re-push once its delay has passed.
 * The banner has no subscribe of its own, so the timer is its only second chance.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole banner notification is published.
 */
[[nodiscard]] bool consume_banner_repush(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept {
    if (!session.bannerRepushArmed || session.bannerRepushRoot == 0
        || GetTickCount64() < session.bannerRepushDueTick) {
        return false;
    }
    // Nothing is owed while the account owns no character to name. The arm stays set, because it
    // is the banner's only second chance.
    if (state::account::banner_character_soid(state::account_snapshot()) == 0) {
        return false;
    }
    touchesScratch = true;

    // The same body the subscribe answer builds, so the version and this host's mirror stay in
    // step. `append_banner_notification` fixes the version at zero and a pick has moved past it.
    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kBannerFamilyType;
    subscription.familyRootSoid = session.bannerRepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState bannerAfter{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     bannerAfter,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    // The frame is committed here, so the recorded delivery and the arm are committed with it.
    if (valid(bannerAfter)) {
        session.queuez = bannerAfter;
    }
    session.bannerRepushArmed = false;
    report_repush("banner_repush", framedSize);
    return true;
}

/**
 * Sends the owed family-two re-push once its delay has passed.
 *
 * The family-two snapshot is built when the peer subscribes, so the emblem it carries is only
 * correct as of that moment. An equip into the emblem slot leaves it stale, and the Client
 * resolves that account-keyed object as *the* account emblem -- so the display stays pinned to
 * whatever was worn at subscribe time while the equip itself keeps succeeding. This republishes
 * the live value against the root the subscribe was answered with.
 *
 * **One attempt, spent whether or not it lands.** The arm is cleared before the frame is built, so
 * a refusal cannot leave this re-arming every tick; this file records that a boot-shaped replay
 * repeated after the ladder has moved took the connection down.
 *
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole family-two notification is published.
 */
[[nodiscard]] bool consume_social_roster_repush(Session& session,
                                                Scratch& scratch,
                                                std::span<std::byte> response,
                                                std::size_t& written,
                                                bool& touchesScratch) noexcept {
    if (!session.socialRosterRepushArmed || session.socialRosterRepushRoot == 0
        || GetTickCount64() < session.socialRosterRepushDueTick) {
        return false;
    }
    // Spent up front, so no path below can leave it owed.
    session.socialRosterRepushArmed = false;
    touchesScratch = true;

    // The same body the subscribe answer builds, rebuilt against current State so the emblem it
    // carries is the one now worn.
    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kSocialRosterFamilyType;
    subscription.familyRootSoid = session.socialRosterRepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState rosterAfter{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     rosterAfter,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=social_roster_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (valid(rosterAfter)) {
        session.queuez = rosterAfter;
    }
    report_repush("social_roster_repush", framedSize);
    return true;
}

/**
 * Re-derives the selected character's appearance and roster once the ability-bucket rebuild owed
 * by a subclass selection has landed. The refresh sent inline with the opcode-801 response can
 * still carry empty buckets, because that rebuild runs off the Client content-extraction pump.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when at least one owed record refreshes.
 */
[[nodiscard]] bool consume_ability_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    if (!session.abilityRefreshArmed || GetTickCount64() < session.abilityRefreshDueTick) {
        return false;
    }
    // Nothing is owed until a family that reads abilities is subscribed. The arm stays set, the
    // same way the banner re-push below keeps its own.
    if (!session.queuez.family0Active && !session.queuez.family3Active) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState current = session.queuez;
    bool wrote = false;
    if (current.family0Active) {
        queuez::SessionState appearanceAfter{};
        if (push::append_account_resync_appearance_notification(scratch,
                                                                current,
                                                                state::bap().sessionKey,
                                                                nextSendNonce,
                                                                scratch.framed,
                                                                framedSize,
                                                                appearanceAfter)) {
            current = appearanceAfter;
            wrote = true;
        }
    }
    if (current.family3Active) {
        queuez::SessionState rosterAfter{};
        if (push::append_account_resync_roster_notification(scratch,
                                                            current,
                                                            state::bap().sessionKey,
                                                            nextSendNonce,
                                                            scratch.framed,
                                                            framedSize,
                                                            rosterAfter)) {
            current = rosterAfter;
            wrote = true;
        }
    }
    if (!wrote || framedSize == 0 || framedSize > response.size() || !queuez::valid(current)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=ability_refresh result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = current;
    // The frame is committed here, so the arm is committed with it. Disarming any earlier drops
    // the owed refresh on a transient encode failure.
    session.abilityRefreshArmed = false;
    report_repush("ability_refresh", framedSize);
    return true;
}

} // namespace

/**
 * Sends the owed Family-4 re-push once its delay has passed.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole Family-4 notification is published.
 */
bool consume_deferred(Session& session,
                      Scratch& scratch,
                      std::span<std::byte> response,
                      std::size_t& written,
                      bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated) {
        return false;
    }
    if (consume_account_resync(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    // A failed resync remains armed and blocks unrelated deferred output until it can be retried.
    if (session.accountResyncArmed) {
        return false;
    }
    if (consume_ability_refresh(session, scratch, response, written, touchesScratch)) {
        return true;
    }
    if (!session.family4RepushArmed || session.family4RepushRoot == 0
        || GetTickCount64() < session.family4RepushDueTick) {
        return consume_social_roster_repush(session, scratch, response, written, touchesScratch)
               || consume_banner_repush(session, scratch, response, written, touchesScratch)
               || push::activity::consume_activity_keepalive(
                   session, scratch, response, written, touchesScratch);
    }
    // One attempt is owed, and it is spent whether or not it lands.
    touchesScratch = true;

    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kAccountFamilyType;
    subscription.familyRootSoid = session.family4RepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     after,
                                     armsRepush,
                                     armsBannerRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        // Neither failure clears on a retry. Holding the arm starves the keepalive, and the client
        // drops the activity session once the keepalive stops.
        session.family4RepushArmed = false;
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         framedSize == 0 ? "ev=queuez stage=repush result=fail reason=encode"
                                         : "ev=queuez stage=repush result=fail reason=capacity");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (queuez::valid(after)) {
        session.queuez = after;
    }
    session.family4RepushArmed = false;
    report_repush("repush", framedSize);
    return true;
}

} // namespace sunrise::server::bap::encrypted
