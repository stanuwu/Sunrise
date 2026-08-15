#include <Windows.h>

#include "../../../../../state/account/account_state.h"
#include "../../../../../state/activity/defaults/activity_defaults_snapshot.h"
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
/** Default transition token. A client value from message 22 replaces it. */
constexpr std::uint8_t kDefaultTransitionToken = 1;

} // namespace

/** Seeds the membership identity from the join when no identity message has arrived. */
bool seed_identity(std::uint64_t sessionId,
                   std::uint64_t memberKey,
                   std::uint64_t characterSoid) noexcept {
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
    state::activity::membership::PendingMutation mutation{};
    const bool seeded = state::activity::membership::prepare_identity(sessionId, identity, mutation)
                        && state::activity::membership::commit(mutation);
    SecureZeroMemory(&mutation, sizeof mutation);
    return seeded;
}

/** Seeds the transition token when nothing has published one. */
bool seed_transition_token(std::uint64_t sessionId) noexcept {
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    state::activity::membership::AuthoritativeUpdate update{};
    // No teleport is published here. It goes out only when the client's own authoritative-data
    // message supplies one, because a fabricated teleport is a transition the client never asked
    // for. The token fills every member lane of all 64 region records. Zero is never sent.
    update.hasTransitionToken = true;
    update.transitionToken = kDefaultTransitionToken;
    state::activity::membership::PendingMutation mutation{};
    const bool seeded =
        state::activity::membership::prepare_authoritative(sessionId, update, mutation)
        && state::activity::membership::commit(mutation);
    SecureZeroMemory(&mutation, sizeof mutation);
    return seeded;
}

} // namespace sunrise::server::bap::encrypted::push::activity
