#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "account_handle.h"
#include "account_state.h"

namespace sunrise::state {

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

[[nodiscard]] AccountHandle bound_account() noexcept;
/** Public projections cannot access private SQLite, even when they describe the host player. */
[[nodiscard]] bool local_account_access() noexcept;
[[nodiscard]] std::size_t account_count() noexcept;
[[nodiscard]] bool account_handle_for_token(std::span<const std::byte> token,
                                            AccountHandle& handle,
                                            bool* attachedNow = nullptr) noexcept;
[[nodiscard]] bool account_handle_for_soid(std::uint64_t primarySoid,
                                           AccountHandle& handle) noexcept;
[[nodiscard]] bool account_handle_for_platform(std::uint64_t platformId,
                                               AccountHandle& handle) noexcept;
/** Native translation may carry either the complete platform identity or an existing account key.
 */
[[nodiscard]] bool account_handle_for_identity(std::uint64_t identity,
                                               AccountHandle& handle) noexcept;
/** Shared-service object resolution includes each published character, never a default player. */
[[nodiscard]] AccountHandle account_for_public_root(std::uint64_t objectSoid) noexcept;
[[nodiscard]] std::uint64_t account_primary_soid(AccountHandle handle) noexcept;
[[nodiscard]] AccountState account_snapshot(AccountHandle handle) noexcept;
[[nodiscard]] bool account_snapshot(AccountHandle handle, AccountState& output) noexcept;
[[nodiscard]] bool
account_character_class(AccountHandle handle, std::size_t index, CharacterClass& value) noexcept;
[[nodiscard]] bool
set_selected_character(AccountHandle handle, std::uint64_t characterSoid, bool& changed) noexcept;

} // namespace sunrise::state
