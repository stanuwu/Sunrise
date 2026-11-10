/**
 * Automatic pursuit completion: a held pursuit whose contract completes automatically is redeemed
 * by the server once its objectives are done, through the same grant a redemption request uses.
 */
#include <memory>
#include <mutex>
#include <new>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {

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
    const std::unique_ptr<AccountState> account(new (std::nothrow) AccountState{});
    reason = "saved_state";
    if (!account || !investment::store::read_account(*account)) {
        return PursuitRedemption::notReady;
    }
    reason = "character";
    const std::size_t characterIndex = runtime::detail::selected_character_index(*account);
    if (characterIndex >= account->characterCount) {
        return PursuitRedemption::notRedeemable;
    }
    const auto& held = account->characters[characterIndex].inventory;
    auto result = PursuitRedemption::notRedeemable;
    const char* notReadyReason = nullptr;
    while (cursor < held.count) {
        const auto& source = held.values[cursor++];
        build_data::items::Definition item{};
        build_data::pursuits::Definition contract{};
        if (!build_data::find_item_definition_hash(source.definitionHash, item)
            || item.bucketId != build_data::items::kPursuitBucketId
            || !build_data::find_pursuit_definition(item.definitionIndex, contract)
            || !contract.automaticCompletion) {
            continue;
        }
        const PursuitRedemption prepared = prepare_pursuit_redemption(
            source.instanceSoid, source.definitionHash, nowSeconds, seed, mutation, &reason);
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
        core::log::writef(core::log::Channel::state,
                          core::log::Level::info,
                          "ev=pursuit_completion item=%u instance=%llu result=%s reason=%s",
                          static_cast<unsigned>(item.definitionIndex),
                          static_cast<unsigned long long>(source.instanceSoid),
                          prepared == PursuitRedemption::notReady ? "not_ready" : "refused",
                          reason != nullptr ? reason : "unavailable");
    }
    reason = result == PursuitRedemption::notReady ? notReadyReason : nullptr;
    return result;
}

} // namespace sunrise::state
