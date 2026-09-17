#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../network/peer_publication.h"
#include "account_handle.h"
#include "account_state.h"

namespace sunrise::state::account::profiles {

[[nodiscard]] bool valid(const AccountState& profile) noexcept;
void reset(std::uint64_t localSoid = 0) noexcept;
/** Caller has retired every session and directory reference to this exact remote owner. */
[[nodiscard]] bool release(AccountHandle handle, std::uint64_t expectedPrimarySoid) noexcept;
[[nodiscard]] std::size_t count() noexcept;
/** The caller has resolved the session token; enrollment creates no character or equipment. */
[[nodiscard]] bool reserve(std::uint64_t primarySoid,
                           std::span<const std::byte> token,
                           AccountHandle& handle,
                           bool* attachedNow = nullptr) noexcept;
[[nodiscard]] bool find(std::uint64_t primarySoid, AccountHandle& handle) noexcept;
/** Resolves a published platform identity or an account/character object without a fallback. */
[[nodiscard]] bool find_platform(std::uint64_t platformId, AccountHandle& handle) noexcept;
[[nodiscard]] bool find_owner(std::uint64_t objectSoid, AccountHandle& handle) noexcept;
[[nodiscard]] bool find_token(std::span<const std::byte> token, AccountHandle& handle) noexcept;
[[nodiscard]] std::uint64_t primary_soid(AccountHandle handle) noexcept;
/**
 * Validates and atomically replaces one public image in fixed storage for its enrolled owner.

 * * Refusal preserves content and generations. Copies under the cache SRW lock; never allocates
 *
 * or calls the database, BAP, or client callbacks. The input must remain stable during the call.

 */
[[nodiscard]] bool publish(AccountHandle handle, const AccountState& profile) noexcept;
/** An absent profile or invalid handle clears output; it never falls back to the local account. */
[[nodiscard]] bool snapshot(AccountHandle handle, AccountState& output) noexcept;
[[nodiscard]] std::uint32_t generation(AccountHandle handle) noexcept;
/** Changes only when the selected character or public player name changes. */
[[nodiscard]] std::uint32_t membership_generation(AccountHandle handle) noexcept;
[[nodiscard]] std::uint64_t banner_character(AccountHandle handle) noexcept;
/** The actual selected character, with no banner fallback. */
[[nodiscard]] std::uint64_t selected_character(AccountHandle handle) noexcept;
/** Copies the name only if this remains the owner's actual selected character. */
[[nodiscard]] bool selected_member_name(AccountHandle handle,
                                        std::uint64_t characterSoid,
                                        std::array<char, kDisplayNameCapacity>& name) noexcept;
/** Invalidates the public projection after local account or session-overlay changes. */
void local_changed() noexcept;
[[nodiscard]] std::uint64_t local_generation() noexcept;
/** Ephemeral local native publication; private persistence never stores this report. */
[[nodiscard]] bool publish_local_presence(AccountHandle owner,
                                          const social::NativePresence& value) noexcept;
[[nodiscard]] social::NativePresence local_presence() noexcept;
/** Only the owner's selected character publication; absent or stale reports return empty. */
[[nodiscard]] social::NativePresence native_presence(AccountHandle owner) noexcept;
/** Previously published self endpoint; a joined fireteam's leader descriptor cannot replace it. */
[[nodiscard]] std::size_t
peer_endpoints(AccountHandle owner, std::span<network::PeerPublication::Endpoint> output) noexcept;
/** A disconnected account retains its profile but withdraws its native hosting publication. */
void clear_remote_presence(AccountHandle owner) noexcept;
/** Invalidates projections after a committed public service dependency changes. */
void service_changed(AccountHandle owner) noexcept;
[[nodiscard]] std::uint32_t public_generation() noexcept;

} // namespace sunrise::state::account::profiles
