#include "queuez_investment_push.h"

#include <Windows.h>

#include <memory>
#include <new>
#include <span>
#include <utility>
#include <variant>

#include "../../../../core/runtime/server_clock.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/investment/store_internal.h"
#include "../../../../state/runtime/pursuit_edit.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../web_service/web_service_actions.h"
#include "../transactions/service_outcome_commit.h"
#include "queuez_item_grant_push.h"
#include "queuez_outcome_staging.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {

/** Stages the character upsert, then encodes the item, character and account the change names. */
bool stage_changed_objects(const state::AccountEditChange& changed,
                           Session& session,
                           Scratch& scratch,
                           StagedFrame& frame,
                           const char*& reason) noexcept {
    queuez::EquipmentSwap update{};
    if (!queuez::stage_equipment_swap(session.queuez, changed.characterSoid, update)) {
        reason = "inventory_stage";
        return false;
    }
    frame.sendNonce = session.sendNonce;
    const auto presentationRows = active_acquisition_presentation_rows(session);
    const bool encoded =
        changed.itemInstanceSoid != 0
            ? push::append_item_values_refresh_notification(scratch,
                                                            update,
                                                            changed.itemInstanceSoid,
                                                            changed.accountRows,
                                                            presentationRows,
                                                            session.sessionKey,
                                                            frame.sendNonce,
                                                            scratch.framed,
                                                            frame.size)
        : changed.accountRows
            ? push::append_account_refresh_notification(scratch,
                                                        update,
                                                        presentationRows,
                                                        session.sessionKey,
                                                        frame.sendNonce,
                                                        scratch.framed,
                                                        frame.size)
            : push::append_selected_character_refresh_notification(scratch,
                                                                   update,
                                                                   changed.accountSoid,
                                                                   changed.characterIndex,
                                                                   presentationRows,
                                                                   session.sessionKey,
                                                                   frame.sendNonce,
                                                                   scratch.framed,
                                                                   frame.size);
    if (!encoded || frame.size == 0) {
        reason = "inventory_encode";
        return false;
    }
    middleware::secure_channel::advance_nonce(frame.sendNonce);
    frame.after = update.after;
    return true;
}

namespace {

using state::GrantedRowArray;
using state::QuantityEdit;

/** Commits the operation's savepoint, then sends its frame. A failed commit sends nothing. */
[[nodiscard]] InvestmentTransition
commit_and_publish(state::investment::store::Transaction& transaction,
                   const StagedFrame& frame,
                   Session& session,
                   Scratch& scratch,
                   std::span<std::byte> response,
                   std::size_t& written) noexcept {
    if (!transaction.commit()) {
        return refused_transition("transaction");
    }
    publish_staged_frame(frame, session, scratch, response, written);
    return {.state = InvestmentReceiptState::published};
}

/** Encodes the objects one State edit changed, commits the savepoint, then sends the frame. */
[[nodiscard]] InvestmentTransition
publish_changed_objects(state::investment::store::Transaction& transaction,
                        const state::AccountEditChange& change,
                        Session& session,
                        Scratch& scratch,
                        std::span<std::byte> response,
                        std::size_t& written,
                        bool& touchesScratch) noexcept {
    touchesScratch = true;
    StagedFrame frame{};
    const char* reason = nullptr;
    if (!stage_changed_objects(change, session, scratch, frame, reason)) {
        return refused_transition(reason);
    }
    return commit_and_publish(transaction, frame, session, scratch, response, written);
}

/** @return The change for one account-row edit. An account refresh needs no character index. */
[[nodiscard]] state::AccountEditChange account_row_change(const InvestmentTarget& target) noexcept {
    return {target.accountSoid, target.characterSoid, 0, true};
}

/**
 * Stages one service outcome's frame with the staging the opcode handlers use, commits its State
 * mutation into the operation's savepoint, then commits the savepoint and sends the frame.
 */
[[nodiscard]] InvestmentTransition
publish_outcome(state::investment::store::Transaction& transaction,
                ServiceOutcome& outcome,
                Session& session,
                Scratch& scratch,
                std::span<std::byte> response,
                std::size_t& written,
                bool& touchesScratch) noexcept {
    touchesScratch = true;
    StagedFrame frame{};
    frame.sendNonce = session.sendNonce;
    const auto presentationRows = active_acquisition_presentation_rows(session);
    queuez::StagedPublication publication{};
    if (!queuez::stage_service_outcome(scratch,
                                       session.queuez,
                                       outcome,
                                       !presentationRows.empty(),
                                       presentationRows,
                                       session.sessionKey,
                                       frame.sendNonce,
                                       scratch.framed,
                                       frame.size,
                                       publication)
        || frame.size == 0 || !publication.hasState) {
        return refused_transition("inventory_encode");
    }
    frame.after = publication.after;
    transactions::Publication committed{};
    const char* reason = nullptr;
    if (!transactions::commit(outcome, committed, reason)) {
        return refused_transition(reason != nullptr ? reason : "inventory_commit");
    }
    return commit_and_publish(transaction, frame, session, scratch, response, written);
}

/**
 * Grants the next step of an item through stage_item_grant, as world rewards are granted. Each
 * poll grants one instance or stack, so each step shows its own toast and overflows on its own.
 */
[[nodiscard]] InvestmentTransition
publish_operation(const GrantItemOperation& request,
                  const InvestmentTarget& /*target*/,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    state::build_data::items::Definition item{};
    if (!state::build_data::find_item_definition_hash(request.definitionHash, item)) {
        return refused_transition("item_definition");
    }
    StagedItemGrant staged{};
    const char* reason = nullptr;
    const std::int32_t step =
        state::item_grant_step(item.definitionIndex, static_cast<std::int32_t>(request.quantity));
    const ItemGrant grant{item.definitionIndex, step};
    switch (stage_item_grant(grant, session, scratch, response, touchesScratch, staged, reason)) {
    case ItemGrantStage::committed:
        break;
    case ItemGrantStage::notReady:
        // A catalog or a saved read was not ready, so a later poll tries the step again.
        return deferred_transition(InvestmentWait::catalog);
    case ItemGrantStage::waiting:
    case ItemGrantStage::deferred:
    case ItemGrantStage::unresolvable:
    case ItemGrantStage::failed:
        // The lane runs only with a whole-frame buffer, so a grant never waits for one. A grant
        // that State cannot draw or place is refused for resubmission.
        return refused_transition(reason != nullptr ? reason : "transaction");
    }
    // An item with no room anywhere would be dropped, so the grant is refused and its savepoint is
    // never committed.
    if (!staged.placed) {
        return refused_transition("no_room");
    }
    if (!transaction.commit()) {
        return refused_transition("transaction");
    }
    // A step without a frame finishes the grant, so the rest of its quantity is never granted.
    if (!publish_item_grant(staged, session, scratch, response, written)) {
        return {.state = InvestmentReceiptState::committedUnpublished,
                .reason = staged.unpublished,
                .grantedQuantity = static_cast<std::uint32_t>(step)};
    }
    const auto granted = static_cast<std::uint32_t>(step);
    return {.state = granted < request.quantity ? InvestmentReceiptState::queued
                                                : InvestmentReceiptState::published,
            .grantedQuantity = granted};
}

/** Removes one unequipped instance through the dismantle opcode's staging, crediting nothing. */
[[nodiscard]] InvestmentTransition
publish_item_removal(const QuantityEdit& edit,
                     state::investment::store::Transaction& transaction,
                     Session& session,
                     Scratch& scratch,
                     std::span<std::byte> response,
                     std::size_t& written,
                     bool& touchesScratch) noexcept {
    ServiceOutcome outcome{};
    auto* removal = emplace_transaction<ItemDismantleTransaction>(outcome);
    if (removal == nullptr) {
        return refused_transition("allocation");
    }
    removal->pending.reset(new (std::nothrow) state::PendingItemDismantle);
    if (removal->pending == nullptr) {
        return refused_transition("allocation");
    }
    const char* reason = nullptr;
    if (!state::prepare_item_removal(edit.characterSoid,
                                     edit.instanceSoid,
                                     edit.definitionHash,
                                     edit.mutationSerial,
                                     edit.expectedQuantity,
                                     *removal->pending,
                                     &reason)) {
        return state_refusal_transition(reason);
    }
    const state::PendingItemDismantle& pending = *removal->pending;
    if (!queuez::stage_item_dismantle(session.queuez,
                                      pending.accountSoid,
                                      pending.characterSoid,
                                      pending.dismantledInstanceSoid,
                                      pending.updates_account(),
                                      removal->update)) {
        return refused_transition("inventory_stage");
    }
    return publish_outcome(
        transaction, outcome, session, scratch, response, written, touchesScratch);
}

/** Replaces one row's quantity. An unequipped instance set to zero is removed instead. */
[[nodiscard]] InvestmentTransition
publish_operation(const QuantityEdit& edit,
                  const InvestmentTarget& /*target*/,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    if (edit.scope == GrantedRowArray::characterItem && edit.quantity == 0) {
        return publish_item_removal(
            edit, transaction, session, scratch, response, written, touchesScratch);
    }
    state::AccountEditChange change{};
    const char* reason = nullptr;
    if (!state::set_quantity(edit, change, &reason)) {
        return state_refusal_transition(reason);
    }
    return publish_changed_objects(
        transaction, change, session, scratch, response, written, touchesScratch);
}

/** Replaces the XP total. The artifact values it changes follow in their own refreshes. */
[[nodiscard]] InvestmentTransition
publish_operation(const SeasonalExperienceOperation& request,
                  const InvestmentTarget& target,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    const char* reason = nullptr;
    const std::uint16_t previousBonus = state::artifact_power_bonus();
    if (!state::replace_seasonal_experience(request.expected, request.replacement, &reason)) {
        return state_refusal_transition(reason);
    }
    const bool bonusMoved = state::artifact_power_bonus() != previousBonus;
    const InvestmentTransition transition = publish_changed_objects(transaction,
                                                                    account_row_change(target),
                                                                    session,
                                                                    scratch,
                                                                    response,
                                                                    written,
                                                                    touchesScratch);
    if (transition.state == InvestmentReceiptState::published) {
        // Family 5 carries the artifact's Power bonus and earned points, both derived from the
        // total. State reports only the bonus, so the refresh is armed for every committed edit.
        bap::arm_artifact_refresh(session);
        if (bonusMoved) {
            bap::arm_social_roster_repush_everywhere();
        }
    }
    return transition;
}

/** Claims one pass row as a client's opcode-2400 request does once it is decoded. */
[[nodiscard]] InvestmentTransition
publish_operation(const SeasonPassClaimOperation& request,
                  const InvestmentTarget& target,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    auto pending = std::unique_ptr<state::PendingSeasonPassReward>(
        new (std::nothrow) state::PendingSeasonPassReward);
    if (!pending) {
        return refused_transition("allocation");
    }
    const char* reason = nullptr;
    if (!web_service::prepare_season_pass_claim(request.rewardIndex, *pending, reason)) {
        return state_refusal_transition(reason);
    }
    if (pending->grant.characterSoid != target.characterSoid) {
        return refused_transition("character_changed");
    }
    ServiceOutcome outcome{};
    auto* claim = body::stage_season_pass_reward(session.queuez, *pending, outcome);
    if (claim == nullptr) {
        return refused_transition("reward_preflight");
    }
    claim->pending = std::move(pending);
    const InvestmentTransition transition =
        publish_outcome(transaction, outcome, session, scratch, response, written, touchesScratch);
    if (transition.state == InvestmentReceiptState::published) {
        bap::arm_acquisition_presentation_hold(session);
    }
    return transition;
}

/** Clears one pass row's claim flag and leaves the items it granted. */
[[nodiscard]] InvestmentTransition
publish_operation(const SeasonPassResetOperation& request,
                  const InvestmentTarget& target,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    const char* reason = nullptr;
    if (!state::reset_season_pass_reward(request.rewardIndex, request.itemHash, &reason)) {
        return state_refusal_transition(reason);
    }
    return publish_changed_objects(transaction,
                                   account_row_change(target),
                                   session,
                                   scratch,
                                   response,
                                   written,
                                   touchesScratch);
}

/** Replaces the saved row that one objective of a held pursuit reads. */
[[nodiscard]] InvestmentTransition
publish_operation(const ObjectiveProgressOperation& request,
                  const InvestmentTarget& target,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    state::AccountEditChange change{};
    const char* reason = nullptr;
    if (!state::set_pursuit_objective(
            {target.characterSoid, request.instanceSoid, request.itemHash, request.mutationSerial},
            request.ordinal,
            request.objectiveHash,
            request.expected,
            request.value,
            core::runtime::server_clock_seconds(),
            change,
            &reason)) {
        return state_refusal_transition(reason);
    }
    return publish_changed_objects(
        transaction, change, session, scratch, response, written, touchesScratch);
}

/** Sets each incomplete required objective to its target. The pursuit is not redeemed. */
[[nodiscard]] InvestmentTransition
publish_operation(const ObjectiveCompletionOperation& request,
                  const InvestmentTarget& target,
                  state::investment::store::Transaction& transaction,
                  Session& session,
                  Scratch& scratch,
                  std::span<std::byte> response,
                  std::size_t& written,
                  bool& touchesScratch) noexcept {
    state::AccountEditChange change{};
    const char* reason = nullptr;
    if (!state::complete_pursuit_objectives(
            {target.characterSoid, request.instanceSoid, request.itemHash, request.mutationSerial},
            core::runtime::server_clock_seconds(),
            change,
            &reason)) {
        return state_refusal_transition(reason);
    }
    return publish_changed_objects(
        transaction, change, session, scratch, response, written, touchesScratch);
}

/** Runs one operation inside its own store savepoint. */
[[nodiscard]] InvestmentTransition run_operation(const InvestmentOperation& operation,
                                                 Session& session,
                                                 Scratch& scratch,
                                                 std::span<std::byte> response,
                                                 std::size_t& written,
                                                 bool& touchesScratch) noexcept {
    state::investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return refused_transition("transaction");
    }
    return std::visit(
        [&](const auto& request) noexcept {
            return publish_operation(request,
                                     operation.target,
                                     transaction,
                                     session,
                                     scratch,
                                     response,
                                     written,
                                     touchesScratch);
        },
        operation.request);
}

/**
 * @return True while this peer still shows a presentation the operation's frame would contradict:
 * a queued XP gain when the total is replaced, or an acquisition overlay a removal frame omits.
 */
[[nodiscard]] bool presentation_pending(const Session& session,
                                        const InvestmentOperation& operation) noexcept {
    if (std::holds_alternative<SeasonalExperienceOperation>(operation.request)) {
        return session.pendingSeasonalExperienceAmount != 0;
    }
    const auto* edit = std::get_if<QuantityEdit>(&operation.request);
    return edit != nullptr && edit->scope == GrantedRowArray::characterItem && edit->quantity == 0
           && GetTickCount64() < session.acquisitionPresentationUntilTick;
}

} // namespace

/** Runs at most one operation per poll, after the peer's resync, world rewards and XP toast. */
bool consume_investment_operation(Session& session,
                                  Scratch& scratch,
                                  std::span<std::byte> response,
                                  std::size_t& written,
                                  bool& touchesScratch) noexcept {
    // Neither a draw nor an uncommitted edit is kept between polls, so an operation runs only
    // when the response can hold a whole frame.
    if (!session.queuez.family4Active || response.size() < scratch.framed.size()) {
        return false;
    }
    InvestmentOperation operation{};
    if (!bap::next_investment_operation_locked(session.queuez.family4RootSoid, operation)) {
        return false;
    }
    InvestmentTransition transition = bap::check_investment_operation(operation);
    if (transition.state == InvestmentReceiptState::queued) {
        transition =
            presentation_pending(session, operation)
                ? deferred_transition(InvestmentWait::none)
                : run_operation(operation, session, scratch, response, written, touchesScratch);
    }
    bap::record_investment_transition_locked(operation.id, transition);
    // A grant step that leaves its operation queued still sent its frame.
    return transition.state == InvestmentReceiptState::published
           || (transition.state == InvestmentReceiptState::queued
               && transition.grantedQuantity != 0);
}

} // namespace sunrise::server::bap::encrypted
