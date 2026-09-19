#include <array>
#include <limits>

#include "../../../../../core/logging/log.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/snapshot.h"
#include "queuez_push_reporting.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/** Resolves private account families by their root; other families use the bound viewer. */
[[nodiscard]] state::AccountHandle
subscription_account(const middleware::queuez::Subscription& subscription) noexcept {
    return subscription.familyType == queuez::kBannerFamilyType
                   || subscription.familyType == queuez::kRosterFamilyType
                   || subscription.familyType == queuez::kAccountFamilyType
               ? state::account_for_subscription_root(subscription.familyRootSoid)
               : state::bound_account();
}

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
    if (!snapshot::prepare_initial(scratch, companion, {}, prepared)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=companion result=fail reason=prepare");
        return false;
    }
    // The record is still state 1 `declared` at this point, and only state 2 accepts a snapshot.
    // The first copy is expected to be rejected and the delayed copy is the one that lands,
    // so a refused staging still sends the frame and still owes the re-push.
    queuez::SessionState staged = before;
    const bool resident = !prepared.family.objects.empty();
    const bool recorded =
        resident && queuez::stage_family4_snapshot(before, prepared.family, staged);
    if (!recorded) {
        staged = before;
    }
    if (!queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=companion result=fail reason=frame");
        return false;
    }
    after = staged;
    return true;
}

} // namespace

/** Appends one current full account snapshot at the peer's next Family-4 version. */
bool append_account_resync_notification(
    Scratch& scratch,
    const queuez::SessionState& before,
    std::span<const queuez::AcquisitionPresentationRow> acquisitionPresentationRows,
    std::span<const std::byte, state::kAesKeySize> key,
    std::array<std::byte, state::kBapNonceSize>& nonce,
    std::span<std::byte> response,
    std::size_t& written,
    queuez::SessionState& after) noexcept {
    const state::ScopedAccountView accountScope(
        state::account_for_subscription_root(before.family4RootSoid));
    after = before;
    ensure_account_canonical();
    if (!queuez::valid(before) || !before.family4Active || before.family4RootSoid == 0
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()) {
        return false;
    }
    snapshot::Prepared prepared{};
    if (!snapshot::prepare_family4_refresh(scratch,
                                           before.family4RootSoid,
                                           before.family4Version + 1,
                                           acquisitionPresentationRows,
                                           prepared)
        || !queuez::stage_family4_refresh(before, prepared.family, after)) {
        return false;
    }
    if (!queuez_frame::append_prepared_frame(scratch, prepared, key, nonce, response, written)) {
        after = before;
        return false;
    }
    return true;
}

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
 * @param armsBannerRepush Receives whether a family-zero body owes its delayed second copy.
 */
void append_queuez_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                const middleware::queuez::Subscription& subscription,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after,
                                bool& armsRepush,
                                bool& armsBannerRepush,
                                bool ownSnapshotAnswered) noexcept {
    const state::ScopedAccountView accountScope(subscription_account(subscription));
    after = before;
    armsRepush = false;
    armsBannerRepush = false;
    if (state::bound_account() == state::kInvalidAccount) {
        return;
    }
    // Runs ahead of the dispatch below; inside one family's builder it would leave the other
    // families describing a different account.
    ensure_account_canonical();
    if (subscription.familyType == queuez::kAccountFamilyType && before.family4Active
        && before.family4Version != queuez::kInitialFamilyVersion) {
        // This mirror of the Client's records is an observation, not an authority on what may be
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
        // A failed mirror check must never send a version-zero roster into an active incremental
        // ladder. The correlated subscription response still goes out without a stale snapshot.
        publish = false;
    }

    snapshot::Prepared prepared{};
    if (subscription.familyType == queuez::kBannerFamilyType) {
        // Family zero's version and flags come from this peer's own ladder, so it is prepared
        // here instead of through the generic initial-snapshot path.
        const state::AccountState account = state::bound_account_snapshot();
        // The first character stands in before any pick. The record accepts a snapshot only in the
        // short window the subscribe opens, so holding the answer for the pick spends that window
        // and the subscription times out. The pick moves the pair afterwards.
        const std::uint64_t selected = state::account::banner_character_soid(account);
        if (selected == 0) {
            stagedAfter.pendingBannerRoot = subscription.familyRootSoid;
            queuez_report::subscription_state("nocharacter");
            after = stagedAfter;
            return;
        }
        stagedAfter.pendingBannerRoot = 0;
        if (!queuez::stage_family0_subscription(
                before, selected, publish, incremental, stagedAfter)) {
            queuez_report::subscription_state("stage_family0");
            stagedAfter = before;
            stagedAfter.pendingBannerRoot = 0;
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
    } else if (!snapshot::prepare_initial(scratch, subscription, {}, prepared)) {
        queuez_report::subscription_failure("prepare");
        return;
    }
    // An empty snapshot is sent without claiming a resident manifest.
    if (subscription.familyType == queuez::kAccountFamilyType && !prepared.family.objects.empty()
        && !queuez::stage_family4_snapshot(before, prepared.family, stagedAfter)) {
        queuez_report::subscription_state("stage_family4");
        stagedAfter = before;
    }
    // An empty full snapshot prunes the family to nothing. Wiping the roster closes the gate the
    // family-zero source list is emitted from. Every other family needs the empty snapshot: it is
    // what moves a record with no body to synced.
    if (prepared.family.objects.empty() && subscription.familyType == queuez::kRosterFamilyType) {
        queuez_frame::clear_object_storage(
            scratch, prepared.rawClearSize, prepared.compressedClearSize);
        queuez_report::subscription_state("empty");
        after = stagedAfter;
        return;
    }
    if (!publish) {
        // Response-only suppression still builds the live snapshot, which names the root.
        queuez_frame::clear_object_storage(
            scratch, prepared.rawClearSize, prepared.compressedClearSize);
        after = stagedAfter;
        return;
    }
    // The reply that answered this subscribe already carries the snapshot, so only the ladder
    // moves here and the companions still follow.
    if (ownSnapshotAnswered) {
        queuez_frame::clear_object_storage(
            scratch, prepared.rawClearSize, prepared.compressedClearSize);
    } else if (!queuez_frame::append_prepared_frame(
                   scratch, prepared, key, nonce, response, written)) {
        queuez_report::subscription_failure("frame");
        return;
    }
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

/** Builds the subscribed family's first snapshot and encodes it as one svc-123 body. */
bool prepare_subscription_answer(Scratch& scratch,
                                 const queuez::SessionState& before,
                                 const middleware::queuez::Subscription& subscription,
                                 std::span<std::byte> body,
                                 std::size_t& bodySize) noexcept {
    bodySize = 0;
    const state::ScopedAccountView accountScope(subscription_account(subscription));
    const bool validAccount = state::bound_account() != state::kInvalidAccount;
    if (validAccount) {
        ensure_account_canonical();
    }
    snapshot::Prepared prepared{};
    bool built = false;
    if (validAccount && subscription.familyType == queuez::kBannerFamilyType) {
        const state::AccountState account = state::bound_account_snapshot();
        const std::uint64_t selected = state::account::banner_character_soid(account);
        if (selected != 0) {
            bool publish = true;
            bool incremental = false;
            queuez::SessionState staged = before;
            if (!queuez::stage_family0_subscription(
                    before, selected, publish, incremental, staged)) {
                staged = before;
                incremental = false;
            }
            built = snapshot::prepare_banner(scratch,
                                             subscription.familyRootSoid,
                                             staged.family0Version,
                                             incremental ? before.family0Character : 0,
                                             prepared);
        }
    } else if (validAccount) {
        // A subscribe establishes a fresh client-side store, so the answer is the live full body
        // even while the push ladder is response-only.
        built = snapshot::prepare_initial(scratch, subscription, {}, prepared);
    }
    middleware::queuez::Family empty{};
    empty.type = subscription.familyType;
    empty.rootSoid = subscription.familyRootSoid;
    empty.flags = middleware::queuez::kFullSnapshotFlag;
    const std::array families{built ? prepared.family : empty};
    const bool encoded = middleware::queuez::encode_update(families, body, bodySize);
    if (built) {
        queuez_frame::clear_object_storage(
            scratch, prepared.rawClearSize, prepared.compressedClearSize);
    }
    if (!encoded) {
        bodySize = 0;
        queuez_report::subscription_failure("answer");
    }
    return encoded;
}

} // namespace sunrise::server::bap::encrypted::push
