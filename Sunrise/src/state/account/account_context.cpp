#include "account_context.h"

#include <algorithm>
#include <cstring>

#include "../../core/settings/settings.h"
#include "../investment/store.h"
#include "../runtime/runtime.h"
#include "account_token.h"
#include "public_profiles.h"

namespace sunrise::state {
namespace {
// Client and process-lifecycle work use the installation's local account. Server dispatch
// binds the authenticated connection explicitly, including invalid unauthenticated handles.
thread_local AccountHandle g_boundAccount = kLocalAccount;
thread_local bool g_publicOnly = false;
} // namespace

ScopedAccount::ScopedAccount(AccountHandle handle, bool publicOnly) noexcept
    : previous_(g_boundAccount), previousPublicOnly_(g_publicOnly) {
    g_boundAccount = handle;
    g_publicOnly = publicOnly || previousPublicOnly_;
}
ScopedAccount::~ScopedAccount() noexcept {
    g_boundAccount = previous_;
    g_publicOnly = previousPublicOnly_;
}
AccountHandle bound_account() noexcept {
    return g_boundAccount;
}

bool local_account_access() noexcept {
    return g_boundAccount == kLocalAccount && !g_publicOnly;
}

ScopedAccountView::ScopedAccountView(AccountHandle handle) noexcept
    : binding_(handle, handle != kLocalAccount || !local_account_access()) {}

bool is_local_account_token(std::span<const std::byte> token) noexcept {
    const auto& local = sign_on().sessionToken;
    return token.size() == local.size()
           && std::any_of(
               local.begin(), local.end(), [](std::byte value) { return value != std::byte{}; })
           && std::equal(token.begin(), token.end(), local.begin());
}

std::size_t account_count() noexcept {
    return account::profiles::count();
}

bool account_handle_for_token(std::span<const std::byte> token,
                              AccountHandle& handle,
                              bool* attachedNow) noexcept {
    handle = kInvalidAccount;
    if (attachedNow) {
        *attachedNow = false;
    }
    if (token.size() != account::kSignOnTokenSize) {
        return false;
    }
    if (is_local_account_token(token)) {
        handle = kLocalAccount;
        return true;
    }
    if (!core::settings::hosts_session()) {
        return false;
    }
    if (account::profiles::find_token(token, handle)) {
        return true;
    }
    const auto primarySoid = account::soid_from_signon_token(token);
    if (primarySoid != 0 && account::profiles::reserve(primarySoid, token, handle, attachedNow)) {
        return true;
    }
    handle = kInvalidAccount;
    return false;
}

bool account_handle_for_soid(std::uint64_t primarySoid, AccountHandle& handle) noexcept {
    handle = kInvalidAccount;
    return account::profiles::find(primarySoid, handle);
}

bool account_handle_for_platform(std::uint64_t platformId, AccountHandle& handle) noexcept {
    handle = kInvalidAccount;
    if (platformId != 0 && platformId == core::settings::get().steam.user.steamId) {
        handle = kLocalAccount;
        return true;
    }
    return account::profiles::find_platform(platformId, handle);
}

AccountHandle account_for_public_root(std::uint64_t objectSoid) noexcept {
    AccountHandle handle = kInvalidAccount;
    if (!account::profiles::find_owner(objectSoid, handle)) {
        return kInvalidAccount;
    }
    return handle;
}

bool local_account_owns_root(std::uint64_t objectSoid) noexcept {
    return local_account_access() && objectSoid != 0
           && account::profiles::primary_soid(kLocalAccount) != 0
           && investment::store::owns_account_root(objectSoid);
}

AccountHandle account_for_subscription_root(std::uint64_t objectSoid) noexcept {
    const auto published = account_for_public_root(objectSoid);
    if (!local_account_access() || (published != kInvalidAccount && published != kLocalAccount)) {
        return published;
    }
    // Local startup does not require a published profile; stale public roots do not grant private
    // access.
    return local_account_owns_root(objectSoid) ? kLocalAccount : kInvalidAccount;
}

bool account_handle_for_identity(std::uint64_t identity, AccountHandle& handle) noexcept {
    if (account_handle_for_platform(identity, handle)) {
        return true;
    }
    AccountHandle candidate = kInvalidAccount;
    if (!account::profiles::find(identity, candidate)
        || (candidate != kLocalAccount && account::profiles::generation(candidate) == 0)) {
        return false;
    }
    handle = candidate;
    return true;
}

std::uint64_t account_primary_soid(AccountHandle handle) noexcept {
    return account::profiles::primary_soid(handle);
}

bool local_account_snapshot(AccountState& output) noexcept {
    if (!local_account_access() || !investment::store::read_account(output)) {
        output = {};
        return false;
    }
    const auto& user = core::settings::get().steam.user;
    output.presence.platformId = user.steamId;
    const auto length =
        (std::min)(std::strlen(user.personaName.data()), output.presence.personaName.size() - 1);
    std::copy_n(user.personaName.begin(), length, output.presence.personaName.begin());
    output.presence.displayName = output.presence.personaName;
    const auto native = account::profiles::local_presence();
    if (native.characterSoid == account::selected_character_soid(output)) {
        output.presence.native = native;
    }
    return true;
}

bool public_account_snapshot(AccountHandle handle, AccountState& output) noexcept {
    return account::profiles::snapshot(handle, output);
}

bool bound_account_snapshot(AccountState& output) noexcept {
    return local_account_access() ? local_account_snapshot(output)
                                  : public_account_snapshot(bound_account(), output);
}

bool account_character_class(AccountHandle handle,
                             std::size_t index,
                             CharacterClass& value) noexcept {
    AccountState account{};
    const bool found = handle == kLocalAccount && local_account_access()
                           ? local_account_snapshot(account)
                           : public_account_snapshot(handle, account);
    if (!found || index >= account.characterCount) {
        return false;
    }
    value = account.characters[index].characterClass;
    return true;
}

bool set_selected_character(AccountHandle handle,
                            std::uint64_t characterSoid,
                            bool& changed) noexcept {
    changed = false;
    return handle == kLocalAccount && set_selected_character(characterSoid, changed);
}

} // namespace sunrise::state
