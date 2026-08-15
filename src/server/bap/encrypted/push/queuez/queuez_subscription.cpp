#include <array>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/snapshot.h"
#include "queuez_push_reporting.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/**
 * Appends the unsolicited Family-4 companion of a Family-3 subscription.
 * @param scratch Lock-owned transform buffers.
 * @param before Queuez state visible to the current BAP peer.
 * @param familyRootSoid Root the Client subscribed for Family 3.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @param after Receives the queuez state published when the companion succeeds.
 * @return True when the companion frame is appended.
 */
[[nodiscard]] bool append_family4_companion(Scratch& scratch,
                                            const queuez::SessionState& before,
                                            std::uint64_t familyRootSoid,
                                            std::span<const std::byte, state::kAesKeySize> key,
                                            std::array<std::byte, state::kBapNonceSize>& nonce,
                                            std::span<std::byte> response,
                                            std::size_t& written,
                                            queuez::SessionState& after) noexcept {
    middleware::queuez::Subscription companion{};
    companion.familyType = queuez::kAccountFamilyType;
    companion.familyRootSoid = familyRootSoid;

    snapshot::Prepared prepared{};
    if (!snapshot::prepare_initial(scratch, companion, prepared)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=companion result=fail reason=prepare");
        return false;
    }
    // The record is still state 1 DECLARED at this point, and only state 2 accepts a snapshot.
    // The first copy is expected to be rejected and the delayed copy is the one that lands,
    // so a refused staging still sends the frame and still owes the re-push.
    queuez::SessionState staged = before;
    const bool resident = !prepared.family.objects.empty();
    const bool recorded =
        resident && queuez::stage_family4_snapshot(before, prepared.family, staged);
    if (!recorded) {
        staged = before;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=companion result=fail reason=frame");
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    after = staged;
    queuez_report::push("companion",
                        queuez::kAccountFamilyType,
                        objectCount,
                        written - beforeBytes,
                        recorded ? 1 : 0);
    return true;
}

} // namespace

/**
 * Stages the snapshots one subscription needs.
 * Every step reports and continues. The subscribe is answered whether or not a frame is built.
 * @param scratch Lock-owned transform buffers.
 * @param before Queuez state visible to the current BAP peer.
 * @param subscription Family the Client picked.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced once per appended frame.
 * @param response Caller-owned output containing the existing response prefix.
 * @param written Existing byte count, updated after each complete push.
 * @param after Receives the queuez state published after caller output is copied.
 * @param armsRepush Receives whether the Family-4 companion owes its delayed second copy.
 */
void append_queuez_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                const middleware::queuez::Subscription& subscription,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after,
                                bool& armsRepush) noexcept {
    after = before;
    armsRepush = false;
    if (subscription.familyType == queuez::kAccountFamilyType && before.family4Active
        && before.family4Version != queuez::kInitialFamilyVersion) {
        // Our mirror of the Client's records is an observation, not an authority on what may be
        // sent. It reports and the frame still goes out. The Client owns the accept decision.
        queuez_report::subscription_state("session");
    }

    bool publish = true;
    bool incremental = false;
    queuez::SessionState stagedAfter = before;
    if (subscription.familyType == queuez::kRosterFamilyType
        && !queuez::stage_family3_subscription(before, subscription, publish, stagedAfter)) {
        queuez_report::subscription_state("stage_family3");
        stagedAfter = before;
        publish = true;
    }

    snapshot::Prepared prepared{};
    if (subscription.familyType == queuez::kBannerFamilyType) {
        // Family zero's version and flags come from this peer's own ladder, so it is prepared
        // here instead of through the generic initial-snapshot path.
        const state::AccountState account = state::account_snapshot();
        std::uint64_t selected = 0;
        for (std::size_t index = 0; index < account.characterCount; ++index) {
            if (account.characters[index].selected) {
                selected = account.characters[index].soid;
            }
        }
        // The pair names one character, so with none selected there is nothing to build yet and
        // the first pick delivers it.
        if (selected == 0) {
            queuez_report::subscription_state("unselected");
            return;
        }
        if (!queuez::stage_family0_subscription(
                before, selected, publish, incremental, stagedAfter)) {
            queuez_report::subscription_state("stage_family0");
            stagedAfter = before;
            incremental = false;
        }
        // The unsolicited pair records its own delivery, so a later explicit subscribe finds the
        // ladder already holding this character. The Client asked, so it is answered regardless.
        publish = true;
        if (!snapshot::prepare_banner(scratch,
                                      subscription.familyRootSoid,
                                      stagedAfter.family0Version,
                                      incremental ? before.family0Character : 0,
                                      prepared)) {
            queuez_report::subscription_failure("prepare_banner");
            return;
        }
    } else if (!snapshot::prepare_initial(scratch, subscription, prepared)) {
        queuez_report::subscription_failure("prepare");
        return;
    }
    // An empty snapshot is sent without claiming a resident manifest.
    if (subscription.familyType == queuez::kAccountFamilyType && !prepared.family.objects.empty()
        && !queuez::stage_family4_snapshot(before, prepared.family, stagedAfter)) {
        queuez_report::subscription_state("stage_family4");
        stagedAfter = before;
    }
    if (!publish) {
        // Response-only suppression still builds the live snapshot, which names the root.
        queuez_frame::clear_object_storage(
            scratch, prepared.rawClearSize, prepared.compressedClearSize);
        after = stagedAfter;
        return;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        queuez_report::subscription_failure("frame");
        return;
    }
    middleware::secure_channel::advance_nonce(nonce);
    queuez_report::push("snapshot",
                        subscription.familyType,
                        objectCount,
                        written - beforeBytes,
                        queuez_report::kNoRecordOutcome);
    after = stagedAfter;

    if (subscription.familyType == queuez::kRosterFamilyType && !stagedAfter.family4Active) {
        queuez::SessionState companionAfter{};
        if (append_family4_companion(scratch,
                                     stagedAfter,
                                     subscription.familyRootSoid,
                                     key,
                                     nonce,
                                     response,
                                     written,
                                     companionAfter)) {
            after = companionAfter;
            armsRepush = true;
        }
        // The banner pair follows family four, last in the burst, which is retail's order.
        const queuez::SessionState bannerBefore = after;
        queuez::SessionState bannerDelivered{};
        if (append_banner_notification(scratch,
                                       bannerBefore,
                                       subscription.familyRootSoid,
                                       key,
                                       nonce,
                                       response,
                                       written,
                                       bannerDelivered)) {
            after = bannerDelivered;
        }
    }
}

} // namespace sunrise::server::bap::encrypted::push
