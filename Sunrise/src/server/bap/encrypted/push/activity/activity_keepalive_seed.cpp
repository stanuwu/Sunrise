#include "../../../../../state/account/account_state.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

/**
 * Member field 1 is a skip test. -1 is the only value that keeps the member, because the client
 * drops it when the field read as unsigned is at or below 0x1FF.
 */
constexpr std::int32_t kMemberSkipTest = -1;
/**
 * Member field 2 rides a bias of 0x80000000, and the captured reference body sends wire 0x7FFFFFFF,
 * which is logical -1. Seeding zero instead cost the ship and the banner.
 */
constexpr std::int32_t kUnsetOpaque = -1;

/** @return The fallback membership identity for one joining client key. */
[[nodiscard]] state::activity::membership::Identity
seed_identity(std::uint64_t memberKey, std::uint64_t characterSoid) noexcept {
    const state::AccountState account = state::bound_account_snapshot();
    state::activity::membership::Identity identity{};
    identity.memberKey = memberKey;
    identity.smallOpaque = kMemberSkipTest;
    identity.signedOpaque = kUnsetOpaque;
    identity.joinIdentity = memberKey;
    identity.accountSoid = account.primarySoid;
    // This is the message that creates the player, so its character must be the one the client
    // signed in on. The selected character is only the fallback for a join that named none.
    identity.opaqueSoid =
        characterSoid != 0 ? characterSoid : state::account::selected_character_soid(account);
    return identity;
}

} // namespace

/** Prepares the fallback membership identity without changing stored State. */
bool prepare_seed_identity(std::uint64_t sessionId,
                           std::uint64_t memberKey,
                           std::uint64_t characterSoid,
                           state::activity::membership::PendingMutation& mutation) noexcept {
    mutation = {};
    if (memberKey == 0) {
        return false;
    }
    return state::activity::membership::prepare_identity(
        sessionId, seed_identity(memberKey, characterSoid), mutation);
}

/** Builds the membership snapshot a first join commits, without reading State. */
bool prepare_join_seed_snapshot(std::uint64_t createdRevision,
                                std::uint64_t memberKey,
                                std::uint64_t characterSoid,
                                state::activity::membership::PendingMutation& mutation) noexcept {
    mutation = {};
    if (memberKey == 0) {
        return false;
    }
    // The join commit clears the record's membership, so the seed commit produces exactly this:
    // the seed identity over cleared state at the initial revision, epoch, and token.
    mutation.snapshot.identity = seed_identity(memberKey, characterSoid);
    mutation.snapshot.revision = state::activity::membership::kInitialRevision;
    mutation.snapshot.epoch = state::activity::membership::session_epoch(createdRevision);
    mutation.snapshot.transitionToken = state::activity::membership::kInitialTransitionToken;
    mutation.hasSnapshot = true;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::activity
