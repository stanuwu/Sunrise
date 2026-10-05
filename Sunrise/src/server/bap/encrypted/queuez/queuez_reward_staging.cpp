/** Reward lane of the queuez service outcome: acquisitions, record rewards and season pass. */

#include "queuez_reward_staging.h"

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"

namespace sunrise::server::bap::encrypted::queuez {
namespace {

/**
 * Stages the character upsert and appended resident an item acquisition promised.
 * The after-image is the one body processing staged against this same peer state.
 */
[[nodiscard]] bool
stage_item_acquisition_push(Scratch& scratch,
                            const ItemAcquisition& acquisition,
                            const state::PendingItemAcquisition& pending,
                            std::span<const AcquisitionPresentationRow> presentationRows,
                            std::span<const std::byte, state::kAesKeySize> key,
                            std::array<std::byte, state::kBapNonceSize>& nonce,
                            std::span<std::byte> response,
                            std::size_t& written,
                            SessionState& after) noexcept {
    if (!push::append_item_acquisition_notification(
            scratch, acquisition, pending, presentationRows, key, nonce, response, written)) {
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    after = acquisition.after;
    return true;
}

/**
 * Stages the account upsert and optional manifest append a profile acquisition promised.
 * The after-image is the one body processing staged against this same peer state.
 */
[[nodiscard]] bool
stage_profile_item_acquisition_push(Scratch& scratch,
                                    const ProfileItemAcquisition& acquisition,
                                    const state::PendingProfileItemAcquisition& pending,
                                    std::span<const std::byte, state::kAesKeySize> key,
                                    std::array<std::byte, state::kBapNonceSize>& nonce,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    SessionState& after) noexcept {
    if (!push::append_profile_item_acquisition_notification(
            scratch, acquisition, pending, key, nonce, response, written)) {
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    after = acquisition.after;
    return true;
}

/** Publishes a reward batch before advancing the nonce and peer revision. */
[[nodiscard]] bool
stage_record_reward_push(Scratch& scratch,
                         const SessionState& before,
                         const RecordRewardGrant& update,
                         const state::PendingRecordRewardGrant& pending,
                         std::span<const AcquisitionPresentationRow> presentationRows,
                         std::span<const std::byte, state::kAesKeySize> key,
                         std::array<std::byte, state::kBapNonceSize>& nonce,
                         std::span<std::byte> response,
                         std::size_t& written,
                         SessionState& after) noexcept {
    if (!push::append_record_reward_notification(
            scratch, before, update, pending, presentationRows, key, nonce, response, written)) {
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    after = update.after;
    return true;
}

} // namespace

/** @return True when an acquisition or reward transaction owns this outcome. */
bool owns_reward_outcome(const ServiceOutcome& outcome) noexcept {
    return transaction_if<ItemAcquisitionTransaction>(outcome) != nullptr
           || transaction_if<ProfileItemAcquisitionTransaction>(outcome) != nullptr
           || transaction_if<RecordRewardGrantTransaction>(outcome) != nullptr
           || transaction_if<SeasonPassRewardTransaction>(outcome) != nullptr;
}

/** Stages the queuez frames one acquisition or reward outcome asks for. */
bool stage_reward_outcome(Scratch& scratch,
                          const SessionState& before,
                          const ServiceOutcome& outcome,
                          std::span<const AcquisitionPresentationRow> presentationRows,
                          std::span<const std::byte, state::kAesKeySize> key,
                          std::array<std::byte, state::kBapNonceSize>& nonce,
                          std::span<std::byte> response,
                          std::size_t& written,
                          SessionState& after) noexcept {
    if (const auto* item = transaction_if<ItemAcquisitionTransaction>(outcome)) {
        // Body processing staged this exact manifest append before encoding the response version.
        // The character and new item objects must both fit or the State insertion is not committed.
        if (item->pending != nullptr
            && stage_item_acquisition_push(scratch,
                                           item->update,
                                           *item->pending,
                                           presentationRows,
                                           key,
                                           nonce,
                                           response,
                                           written,
                                           after)) {
            return true;
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=acquire result=fail");
        return false;
    }
    if (const auto* profile = transaction_if<ProfileItemAcquisitionTransaction>(outcome)) {
        // A source-backed profile append creates one dependency before the account starts naming
        // it. Existing stacks and non-actionable currency rows preserve the complete manifest.
        if (profile->pending != nullptr
            && stage_profile_item_acquisition_push(scratch,
                                                   profile->update,
                                                   *profile->pending,
                                                   key,
                                                   nonce,
                                                   response,
                                                   written,
                                                   after)) {
            return true;
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=profile_acquire result=fail");
        return false;
    }
    if (const auto* record = transaction_if<RecordRewardGrantTransaction>(outcome)) {
        if (record->pending == nullptr
            || !stage_record_reward_push(scratch,
                                         before,
                                         record->update,
                                         *record->pending,
                                         presentationRows,
                                         key,
                                         nonce,
                                         response,
                                         written,
                                         after)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=record_reward result=fail");
            return false;
        }
        return true;
    }
    const auto* seasonPass = transaction_if<SeasonPassRewardTransaction>(outcome);
    if (seasonPass == nullptr || seasonPass->pending == nullptr
        || !stage_record_reward_push(scratch,
                                     before,
                                     seasonPass->update,
                                     seasonPass->pending->grant,
                                     presentationRows,
                                     key,
                                     nonce,
                                     response,
                                     written,
                                     after)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws2400 stage=queuez_reward result=fail");
        return false;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
