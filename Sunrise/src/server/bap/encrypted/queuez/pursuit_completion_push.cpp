#include "pursuit_completion_push.h"

#include <memory>
#include <new>

#include "../../../../core/logging/log.h"
#include "../../../../core/runtime/server_clock.h"
#include "../../../../middleware/crypto/random_bytes.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/investment/store_internal.h"
#include "../../../../state/runtime/runtime.h"
#include "queuez_item_grant_push.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Starts a new sweep when saved state or the contracts moved, or a reported refusal went stale. */
void reset_stale_sweep(Session& session,
                       std::uint64_t investmentRevision,
                       std::uint64_t definitionsRevision) noexcept {
    if (session.pursuitInvestmentRevision == investmentRevision
        && session.pursuitDefinitionsRevision == definitionsRevision
        && (!session.pursuitPublicationFailureReported
            || session.pursuitPublicationVersion == session.queuez.family4Version)) {
        return;
    }
    session.pursuitInvestmentRevision = investmentRevision;
    session.pursuitDefinitionsRevision = definitionsRevision;
    session.pursuitPublicationVersion = session.queuez.family4Version;
    session.pursuitCompletionCursor = 0;
    session.pursuitCompletionScanned = false;
    session.pursuitPublicationFailureReported = false;
}

} // namespace

/** Prepares, stages and encodes before committing, so a refused frame leaves nothing granted. */
bool consume_pursuit_completion(Session& session,
                                Scratch& scratch,
                                std::span<std::byte> response,
                                std::size_t& written,
                                bool& touchesScratch) noexcept {
    if (!session.queuez.family4Active) {
        return false;
    }
    const std::uint64_t investmentRevision = state::investment::store::revision();
    const std::uint64_t definitionsRevision = state::build_data::pursuit_definitions_revision();
    reset_stale_sweep(session, investmentRevision, definitionsRevision);
    // A draw is not kept between polls, so wait for a response that can hold a whole frame.
    if (session.pursuitCompletionScanned || response.size() < scratch.framed.size()) {
        return false;
    }
    const std::unique_ptr<state::PendingRecordRewardGrant> pending(
        new (std::nothrow) state::PendingRecordRewardGrant);
    std::uint64_t seed = 0;
    if (!pending
        || !middleware::crypto::random::fill(std::as_writable_bytes(std::span(&seed, 1)))) {
        return false;
    }
    state::investment::store::Transaction transaction;
    if (!transaction.ready() || state::investment::store::revision() != investmentRevision) {
        return false;
    }
    std::size_t cursor = session.pursuitCompletionCursor;
    const state::PursuitRedemption preparation = state::prepare_next_automatic_pursuit(
        core::runtime::server_clock_seconds(), seed, cursor, *pending);
    if (state::build_data::pursuit_definitions_revision() != definitionsRevision) {
        return false;
    }
    if (preparation != state::PursuitRedemption::prepared) {
        session.pursuitCompletionCursor = 0;
        session.pursuitCompletionScanned = preparation != state::PursuitRedemption::notReady;
        return false;
    }

    touchesScratch = true;
    queuez::RecordRewardGrant update{};
    StagedFrame frame{};
    frame.sendNonce = session.sendNonce;
    const bool staged = queuez::stage_record_reward_grant(session.queuez, *pending, update);
    const bool encoded = staged
                         && push::append_record_reward_notification(scratch,
                                                                    session.queuez,
                                                                    update,
                                                                    *pending,
                                                                    {},
                                                                    session.sessionKey,
                                                                    frame.sendNonce,
                                                                    scratch.framed,
                                                                    frame.size)
                         && frame.size != 0 && frame.size <= response.size();
    if (!encoded) {
        // Staging refuses this peer's manifest, so the sweep moves past the candidate. Encoding can
        // fail transiently, so that candidate stays owed.
        if (!staged) {
            session.pursuitCompletionCursor = cursor;
        }
        if (!session.pursuitPublicationFailureReported) {
            core::log::writef(
                core::log::Channel::server,
                core::log::Level::warn,
                "ev=pursuit stage=automatic_publish result=refused source=%llu reason=%s",
                static_cast<unsigned long long>(pending->redeemedInstanceSoid),
                staged ? "encode" : "stage");
            session.pursuitPublicationFailureReported = true;
        }
        return false;
    }
    const bool presentsReward = pending->rewardCount != 0;
    // Commit consumes the grant. This transaction keeps its XP with it.
    const std::int32_t experience = pending->seasonalExperience;
    if (!state::commit_record_reward(*pending)
        || (experience != 0 && !bap::arm_seasonal_experience_presentation(experience))
        || !transaction.commit()) {
        return false;
    }
    middleware::secure_channel::advance_nonce(frame.sendNonce);
    frame.after = update.after;
    // The redeemed pursuit leaves its row, which moves the rows held for an earlier acquisition.
    session.acquisitionPresentationRows = {};
    session.acquisitionPresentationRowCount = 0;
    session.acquisitionPresentationUntilTick = 0;
    publish_staged_frame(frame, session, scratch, response, written);
    if (presentsReward) {
        bap::arm_acquisition_presentation_hold(session);
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted
