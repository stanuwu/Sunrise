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

std::size_t account_count() noexcept {
    return account::profiles::count();
}

bool account_handle_for_token(std::span<const std::byte> token,
                              AccountHandle& handle,
                              bool* attachedNow) noexcept {
    if (attachedNow) {
        *attachedNow = false;
    }
    if (token.size() != account::kSignOnTokenSize) {
        return false;
    }
    const auto& local = sign_on().sessionToken;
    if (std::equal(token.begin(), token.end(), local.begin())) {
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
    return primarySoid != 0 && account::profiles::reserve(primarySoid, token, handle, attachedNow);
}

bool account_handle_for_soid(std::uint64_t primarySoid, AccountHandle& handle) noexcept {
    return account::profiles::find(primarySoid, handle);
}

bool account_handle_for_platform(std::uint64_t platformId, AccountHandle& handle) noexcept {
    if (platformId != 0 && platformId == core::settings::get().steam.user.steamId) {
        handle = kLocalAccount;
        return true;
    }
    return account::profiles::find_platform(platformId, handle);
}

AccountHandle account_for_public_root(std::uint64_t objectSoid) noexcept {
    // A client already routed its own roots to the local service. Only the shared host resolves
    // foreign roots here; ordinary upstream callers retain their bound local account.
    if (!core::settings::hosts_session()) {
        return bound_account();
    }
    AccountHandle handle = kInvalidAccount;
    if (!account::profiles::find_owner(objectSoid, handle)) {
        return kInvalidAccount;
    }
    return handle;
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

AccountState account_snapshot(AccountHandle handle) noexcept {
    AccountState output{};
    (void)account_snapshot(handle, output);
    return output;
}

bool account_snapshot(AccountHandle handle, AccountState& output) noexcept {
    if (g_publicOnly) {
        return account::profiles::snapshot(handle, output);
    }
    if (handle == kLocalAccount) {
        if (!investment::store::read_account(output)) {
            return false;
        }
        const auto& user = core::settings::get().steam.user;
        output.presence.platformId = user.steamId;
        const auto length = (std::min)(std::strlen(user.personaName.data()),
                                       output.presence.personaName.size() - 1);
        std::copy_n(user.personaName.begin(), length, output.presence.personaName.begin());
        output.presence.displayName = output.presence.personaName;
        const auto native = account::profiles::local_presence();
        if (native.characterSoid == account::selected_character_soid(output)) {
            output.presence.native = native;
        }
    } else if (!account::profiles::snapshot(handle, output)) {
        output.primarySoid = account::profiles::primary_soid(handle);
        return false;
    }
    return true;
}

bool account_character_class(AccountHandle handle,
                             std::size_t index,
                             CharacterClass& value) noexcept {
    const auto account = account_snapshot(handle);
    if (index >= account.characterCount) {
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
