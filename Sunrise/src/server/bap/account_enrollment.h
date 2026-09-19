#pragma once
#include "../../core/settings/settings.h"
#include "../../state/account/account_context.h"
#include "../../state/account/account_token.h"
#include "../../state/account/public_profiles.h"
#include "../../state/runtime/runtime.h"
#include "../../state/social/steam_roster.h"

namespace sunrise::server::bap {

/** Serialized by the BAP lock. Reuses offline slots as needed; failure invalidates both outputs. */
[[nodiscard]] inline bool enroll_account(std::span<const std::byte> token,
                                         state::AccountHandle& handle,
                                         bool& attachedNow) noexcept {
    handle = state::kInvalidAccount;
    attachedNow = false;
    if (core::settings::role() == core::settings::Role::host
        && state::is_local_account_token(token)) {
        handle = state::kLocalAccount;
        return true;
    }
    if (core::settings::hosts_session()) {
        auto& directory = state::social::session_directory();
        state::AccountHandle known = state::kInvalidAccount;
        const bool connected =
            state::account::profiles::find_token(token, known) && directory.link_count(known) != 0;
        std::size_t players = core::settings::role() == core::settings::Role::host ? 1 : 0;
        for (state::AccountHandle i = 1; i < state::account_count(); ++i) {
            if (directory.link_count(i) != 0) {
                ++players;
            }
        }
        if (!connected && players >= core::settings::get().server.maxPlayers) {
            return false;
        }
    }
    if (state::account_handle_for_token(token, handle, &attachedNow)) {
        return true;
    }
    const auto primary = state::account::soid_from_signon_token(token);
    state::AccountHandle existing = state::kInvalidAccount;
    if (primary == 0 || state::account::profiles::find_owner(primary, existing)
        || state::account_count() < state::kAccountCapacity) {
        return false;
    }
    for (state::AccountHandle i = 1; i < state::account_count(); ++i) {
        if (state::account_primary_soid(i) == 0) {
            return false;
        }
    }
    auto& directory = state::social::session_directory();
    for (state::AccountHandle i = 1; i < state::account_count(); ++i) {
        if (directory.link_count(i) != 0) {
            continue;
        }
        const auto previous = state::account_primary_soid(i);
        if (!directory.forget(i) || !state::account::profiles::release(i, previous)) {
            return false;
        }
        return state::account_handle_for_token(token, handle, &attachedNow);
    }
    return false;
}

/** A failed hello must not consume a new account slot before any connection owns it. */
struct AccountEnrollment {
    state::AccountHandle handle{state::kInvalidAccount};
    std::uint64_t primary{};
    bool attachedNow{};
    ~AccountEnrollment() {
        if (attachedNow) {
            static_cast<void>(state::account::profiles::release(handle, primary));
        }
    }
};

} // namespace sunrise::server::bap
