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
    session.bannerRepushArmed = false;
    // Nothing is owed while no character is selected. The pair has no character to name, and the
    // first pick publishes it. Logging that as a failed re-push would be wrong.
    if (state::account::selected_character_soid(state::account_snapshot()) == 0) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState bannerAfter{};
    if (!push::append_banner_notification(scratch,
                                          session.queuez,
                                          session.bannerRepushRoot,
                                          state::bap().sessionKey,
                                          nextSendNonce,
                                          scratch.framed,
                                          framedSize,
                                          bannerAfter)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    // The frame is committed here, so the recorded delivery is committed with it.
    if (valid(bannerAfter)) {
        session.queuez = bannerAfter;
    }
    report_repush("banner_repush", framedSize);
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
    if (!session.family4RepushArmed || session.family4RepushRoot == 0
        || GetTickCount64() < session.family4RepushDueTick) {
        return consume_banner_repush(session, scratch, response, written, touchesScratch)
               || push::activity::consume_activity_keepalive(
                   session, scratch, response, written, touchesScratch);
    }
    // One attempt is owed. Disarm before trying.
    session.family4RepushArmed = false;
    touchesScratch = true;

    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kAccountFamilyType;
    subscription.familyRootSoid = session.family4RepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    bool armsRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     after,
                                     armsRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (queuez::valid(after)) {
        session.queuez = after;
    }
    report_repush("repush", framedSize);
    return true;
}

} // namespace sunrise::server::bap::encrypted
