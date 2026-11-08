#include "queuez_family_repush.h"

#include <Windows.h>

#include <algorithm>

#include "../../../../core/logging/log.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** How one re-push frame ended. */
enum class RepushResult : std::uint8_t { published, encode, capacity };

/**
 * Rebuilds one family's subscribe answer against current State and frames it into `response`.
 * The nonce and the peer's queuez state move only when the frame is copied out.
 * @param familyType Family the peer subscribed to.
 * @param familyRoot Root the peer's subscribe named.
 * @param written Gets the encoded notification size in bytes.
 * @return Published, or why no frame was copied.
 */
[[nodiscard]] RepushResult publish_repush(Session& session,
                                          Scratch& scratch,
                                          std::uint32_t familyType,
                                          std::uint64_t familyRoot,
                                          std::span<std::byte> response,
                                          std::size_t& written) noexcept {
    middleware::queuez::Subscription subscription{};
    subscription.familyType = familyType;
    subscription.familyRootSoid = familyRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    bool armsRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     session.sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     after,
                                     armsRepush);
    if (framedSize == 0) {
        return RepushResult::encode;
    }
    if (framedSize > response.size()) {
        return RepushResult::capacity;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (queuez::valid(after)) {
        session.queuez = after;
    }
    return RepushResult::published;
}

/** @return True when only a short piggyback slot refused the frame, so the next poll fits it. */
[[nodiscard]] bool retry_owed(RepushResult result,
                              const Scratch& scratch,
                              std::span<const std::byte> response) noexcept {
    return result == RepushResult::capacity && response.size() < scratch.framed.size();
}

} // namespace

/** Either failure disarms; holding the arm would starve the keepalive. */
bool consume_family4_repush(Session& session,
                            Scratch& scratch,
                            std::span<std::byte> response,
                            std::size_t& written,
                            bool& touchesScratch) noexcept {
    touchesScratch = true;
    const RepushResult result = publish_repush(
        session, scratch, queuez::kAccountFamilyType, session.family4RepushRoot, response, written);
    // Neither failure clears on a retry. Holding the arm starves the keepalive, and the client
    // drops the activity session once the keepalive stops.
    session.family4RepushArmed = false;
    if (result == RepushResult::encode) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=repush result=fail reason=encode");
    } else if (result == RepushResult::capacity) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=repush result=fail reason=capacity");
    }
    return result == RepushResult::published;
}

/** Keeps the arm only when a short piggyback slot refused the frame. */
bool consume_social_roster_repush(Session& session,
                                  Scratch& scratch,
                                  std::span<std::byte> response,
                                  std::size_t& written,
                                  bool& touchesScratch) noexcept {
    if (!session.socialRosterRepushArmed || session.socialRosterRepushRoot == 0) {
        return false;
    }
    touchesScratch = true;
    const RepushResult result = publish_repush(session,
                                               scratch,
                                               queuez::kSocialRosterFamilyType,
                                               session.socialRosterRepushRoot,
                                               response,
                                               written);
    session.socialRosterRepushArmed = retry_owed(result, scratch, response);
    if (result != RepushResult::published) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=social_roster_repush result=fail");
    }
    return result == RepushResult::published;
}

/** Waits for a character; keeps the arm only when a short slot refused the frame. */
bool consume_banner_repush(Session& session,
                           Scratch& scratch,
                           std::span<std::byte> response,
                           std::size_t& written,
                           bool& touchesScratch) noexcept {
    if (!session.bannerRepushArmed || session.bannerRepushRoot == 0
        || GetTickCount64() < session.bannerRepushDueTick) {
        return false;
    }
    // Retain the arm until the account has a character to name.
    if (state::account::banner_character_soid(state::account_snapshot()) == 0) {
        return false;
    }
    touchesScratch = true;
    // The subscription path keeps the banner version and the host mirror aligned.
    const RepushResult result = publish_repush(
        session, scratch, queuez::kBannerFamilyType, session.bannerRepushRoot, response, written);
    session.bannerRepushArmed = retry_owed(result, scratch, response);
    if (result != RepushResult::published) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_repush result=fail");
    }
    return result == RepushResult::published;
}

} // namespace sunrise::server::bap::encrypted
