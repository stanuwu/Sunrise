/**
 * Automatic pursuit completion: a held pursuit whose contract completes automatically is redeemed
 * by the server once its objectives are done, through the same grant a redemption request uses.
 */
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "saved_selection_runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace {

/**
 * The inventory rows whose refusal is already logged for one saved revision and one contract
 * revision. A sweep that ends not ready runs again on every poll, and its candidates refuse the
 * same way until one of the two revisions moves.
 */
struct ReportedRefusals {
    std::uint64_t savedRevision{};
    std::uint64_t contractRevision{};
    /** Rows before this one are logged. */
    std::size_t rowsReported{};
};

/** Read and written only under the store lock. */
ReportedRefusals g_reportedRefusals{};

/**
 * Logs a candidate that was not judged or was refused, once until the save or the contracts move.
 * @param row The candidate's inventory row, which a sweep visits in rising order.
 */
void report_refusal_once(std::size_t row,
                         std::uint16_t itemIndex,
                         std::uint64_t instanceSoid,
                         PursuitRedemption outcome,
                         const char* reason) noexcept {
    const std::uint64_t saved = investment::store::revision();
    const std::uint64_t contracts = build_data::pursuit_definitions_revision();
    if (g_reportedRefusals.savedRevision != saved
        || g_reportedRefusals.contractRevision != contracts) {
        g_reportedRefusals = {saved, contracts, 0};
    }
    if (row < g_reportedRefusals.rowsReported) {
        return;
    }
    g_reportedRefusals.rowsReported = row + 1;
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=pursuit_completion stage=automatic result=%s reason=%s item=%u "
                      "instance=%llu",
                      outcome == PursuitRedemption::notReady ? "not_ready" : "refused",
                      reason != nullptr ? reason : "unavailable",
                      static_cast<unsigned>(itemIndex),
                      static_cast<unsigned long long>(instanceSoid));
}

} // namespace

/** Candidates are judged in inventory order, so one refused pursuit never blocks a later one. */
PursuitRedemption prepare_next_automatic_pursuit(std::int64_t nowSeconds,
                                                 std::uint64_t seed,
                                                 std::size_t& cursor,
                                                 PendingRecordRewardGrant& mutation,
                                                 const char** refusal) noexcept {
    const char* unused = nullptr;
    auto& reason = refusal != nullptr ? *refusal : unused;
    reason = "not_ready";
    if (!build_data::pursuit_definitions_ready()) {
        return PursuitRedemption::notReady;
    }
    const std::lock_guard lock(investment::store::g_mutex);
    // One read of the account and its banks serves every candidate of the sweep.
    const std::unique_ptr<SavedSelection<AccountState>> saved(
        new (std::nothrow) SavedSelection<AccountState>{});
    reason = "saved_state";
    if (!saved) {
        return PursuitRedemption::notReady;
    }
    const SavedSelectionRead read = read_saved_selection(*saved);
    if (read == SavedSelectionRead::accountStorage || read == SavedSelectionRead::accountInvalid) {
        return PursuitRedemption::notReady;
    }
    reason = "character";
    if (read == SavedSelectionRead::unselected) {
        return PursuitRedemption::notRedeemable;
    }
    // Why no candidate can be judged, if none can. A sweep that finds no candidate still ends.
    const char* unjudged = nullptr;
    if (!runtime::detail::pursuit_redemption_catalogs_ready()) {
        unjudged = "not_ready";
    } else if (read != SavedSelectionRead::read) {
        unjudged = "saved_state";
    }
    const auto& held = saved->account.characters[saved->slot].inventory;
    auto result = PursuitRedemption::notRedeemable;
    const char* notReadyReason = nullptr;
    while (cursor < held.count) {
        const std::size_t row = cursor++;
        const auto& source = held.values[row];
        build_data::items::Definition item{};
        build_data::pursuits::Definition contract{};
        if (!build_data::find_item_definition_hash(source.definitionHash, item)
            || item.bucketId != build_data::items::kPursuitBucketId
            || !build_data::find_pursuit_definition(item.definitionIndex, contract)
            || !contract.automaticCompletion) {
            continue;
        }
        PursuitRedemption prepared = PursuitRedemption::notReady;
        reason = unjudged;
        if (unjudged == nullptr) {
            prepared = runtime::detail::plan_pursuit_redemption(*saved,
                                                                source.instanceSoid,
                                                                source.definitionHash,
                                                                nowSeconds,
                                                                seed,
                                                                mutation,
                                                                reason);
        }
        if (prepared == PursuitRedemption::prepared) {
            return prepared;
        }
        if (prepared == PursuitRedemption::notRedeemable) {
            continue;
        }
        if (prepared == PursuitRedemption::notReady) {
            result = prepared;
            notReadyReason = reason;
        } else if (result == PursuitRedemption::notRedeemable) {
            result = prepared;
        }
        report_refusal_once(row, item.definitionIndex, source.instanceSoid, prepared, reason);
    }
    reason = result == PursuitRedemption::notReady ? notReadyReason : nullptr;
    return result;
}

} // namespace sunrise::state
