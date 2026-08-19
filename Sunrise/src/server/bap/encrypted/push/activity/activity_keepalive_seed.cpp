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
    const state::AccountState account = state::account_snapshot();
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
    return state::activity::membership::prepare_identity(sessionId, identity, mutation);
}

} // namespace sunrise::server::bap::encrypted::push::activity
