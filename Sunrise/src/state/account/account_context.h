#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "account_handle.h"
#include "account_state.h"

namespace sunrise::state {

/** Binds account access and restores the prior scope on exit; nested scopes cannot regain
 * private access from a remote, invalid or public-only caller. */
class ScopedAccount {
public:
    explicit ScopedAccount(AccountHandle handle, bool publicOnly = false) noexcept;
    ~ScopedAccount() noexcept;
    ScopedAccount(const ScopedAccount&) = delete;
    ScopedAccount& operator=(const ScopedAccount&) = delete;
    ScopedAccount(ScopedAccount&&) = delete;
    ScopedAccount& operator=(ScopedAccount&&) = delete;

private:
    AccountHandle previous_;
    bool previousPublicOnly_;
};

/** Selects a read view without granting private access beyond the caller's scope. */
class ScopedAccountView {
public:
    explicit ScopedAccountView(AccountHandle handle) noexcept;

private:
    ScopedAccount binding_;
};

/** @return Explicit request binding, or the installation account outside server dispatch. */
[[nodiscard]] AccountHandle bound_account() noexcept;
/** Public projections cannot access private SQLite, even when they describe the host player. */
[[nodiscard]] bool local_account_access() noexcept;
/** Matches an initialized local sign-on token without enrolling an account. */
[[nodiscard]] bool is_local_account_token(std::span<const std::byte> token) noexcept;
/** @return Number of enrolled account profiles. */
[[nodiscard]] std::size_t account_count() noexcept;
/** Resolves or enrolls a sign-on token; failure clears handle and attachedNow. */
[[nodiscard]] bool account_handle_for_token(std::span<const std::byte> token,
                                            AccountHandle& handle,
                                            bool* attachedNow = nullptr) noexcept;
/** Finds an enrolled account; failure sets handle to kInvalidAccount. */
[[nodiscard]] bool account_handle_for_soid(std::uint64_t primarySoid,
                                           AccountHandle& handle) noexcept;
/** Finds a configured or published platform identity; failure invalidates handle. */
[[nodiscard]] bool account_handle_for_platform(std::uint64_t platformId,
                                               AccountHandle& handle) noexcept;
/**
 * Native translation accepts a platform identity or published account key; failure
 * invalidates handle.
 */
[[nodiscard]] bool account_handle_for_identity(std::uint64_t identity,
                                               AccountHandle& handle) noexcept;
/** Resolves enrolled account roots and published character roots; unknown roots stay invalid. */
[[nodiscard]] AccountHandle account_for_public_root(std::uint64_t objectSoid) noexcept;
/** Resolves a subscription under the current scope; private local roots must exist in SQLite. */
[[nodiscard]] AccountHandle account_for_subscription_root(std::uint64_t objectSoid) noexcept;
/** Checks initialized local ownership without loading an account; false in public/remote scopes. */
[[nodiscard]] bool local_account_owns_root(std::uint64_t objectSoid) noexcept;
/** @return The account's primary sign-on id, or zero for an out-of-range handle. */
[[nodiscard]] std::uint64_t account_primary_soid(AccountHandle handle) noexcept;
/**
 * Reads only the installation's selected character; no selection is a successful zero. A
 * denied scope or failed storage clears output and returns false. No banner fallback.
 */
[[nodiscard]] bool local_selected_character_soid(std::uint64_t& output) noexcept;
/**
 * Reads private SQLite and local presence only in a local, non-public scope. Failure clears
 * output.
 */
[[nodiscard]] bool local_account_snapshot(AccountState& output) noexcept;
/** Reads the public cache. Before publication, returns false with only the enrolled primary SOID;
 * invalid handles return false with empty output. */
[[nodiscard]] bool public_account_snapshot(AccountHandle handle, AccountState& output) noexcept;
/**
 * Reads the bound account with the scope's visibility. An unpublished public account retains
 * only its enrolled primary SOID and returns false; an invalid scope clears output.
 */
[[nodiscard]] bool bound_account_snapshot(AccountState& output) noexcept;
/** Reads a character class without granting private access to a different account. */
[[nodiscard]] bool
account_character_class(AccountHandle handle, std::size_t index, CharacterClass& value) noexcept;

} // namespace sunrise::state
