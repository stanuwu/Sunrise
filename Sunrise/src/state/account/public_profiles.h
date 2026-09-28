#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../network/peer_publication.h"
#include "account_handle.h"
#include "account_state.h"

namespace sunrise::state::account::profiles {

/**
 * @return True when the platform id, its derived primary soid, presence flags and text
 * fields, the character list, and the native report are all well-formed and mutually
 * consistent; a nonzero native character soid must name one of the listed characters.
 */
[[nodiscard]] bool valid(const AccountState& profile) noexcept;
/**
 * Clears every entry. `localSoid` reseeds slot 0 as this installation's own account;
 * omitted, the local slot is left absent too, as at shutdown.
 */
void reset(std::uint64_t localSoid = 0) noexcept;
/** Caller has retired every session and directory reference to this exact remote owner. */
[[nodiscard]] bool release(AccountHandle handle, std::uint64_t expectedPrimarySoid) noexcept;
/** Slot iteration bound, including local slot zero and released holes; not a live account count. */
[[nodiscard]] std::size_t count() noexcept;
/** The caller has resolved the session token; enrollment creates no character or equipment. */
[[nodiscard]] bool reserve(std::uint64_t primarySoid,
                           std::span<const std::byte> token,
                           AccountHandle& handle,
                           bool* attachedNow = nullptr) noexcept;
/** @return True when any entry, including the local slot, carries this primary soid. */
[[nodiscard]] bool find(std::uint64_t primarySoid, AccountHandle& handle) noexcept;
/** Resolves a published remote platform identity; leaves the handle unchanged on refusal. */
[[nodiscard]] bool find_platform(std::uint64_t platformId, AccountHandle& handle) noexcept;
/** Resolves an enrolled account or published character; leaves the handle unchanged on refusal. */
[[nodiscard]] bool find_owner(std::uint64_t objectSoid, AccountHandle& handle) noexcept;
/** Resolves an enrolled remote token; leaves the handle unchanged on refusal. */
[[nodiscard]] bool find_token(std::span<const std::byte> token, AccountHandle& handle) noexcept;
/** Returns zero for an invalid or released slot. */
[[nodiscard]] std::uint64_t primary_soid(AccountHandle handle) noexcept;
/**
 * Validates and atomically replaces one public image in fixed storage for its enrolled owner.
 * Refusal preserves content and generations. Copies under the cache SRW lock; never
 * allocates, and never calls the database, BAP or client callbacks. The input must remain
 * stable for the duration of the call.
 */
[[nodiscard]] bool publish(AccountHandle handle, const AccountState& profile) noexcept;
/** An absent profile or invalid handle clears output; it never falls back to the local account. */
[[nodiscard]] bool snapshot(AccountHandle handle, AccountState& output) noexcept;
/** Change token for an enrolled slot; zero when absent. Compare for equality, not ordering. */
[[nodiscard]] std::uint32_t generation(AccountHandle handle) noexcept;
/** Tracks enrollment reuse, selected character and public name changes; zero when absent. */
[[nodiscard]] std::uint32_t membership_generation(AccountHandle handle) noexcept;
/** Selected character or first-character fallback; zero without a published character. */
[[nodiscard]] std::uint64_t banner_character(AccountHandle handle) noexcept;
/** The actual selected character, with no banner fallback. */
[[nodiscard]] std::uint64_t selected_character(AccountHandle handle) noexcept;
/** Copies the name only if this remains the owner's actual selected character. */
[[nodiscard]] bool selected_member_name(AccountHandle handle,
                                        std::uint64_t characterSoid,
                                        std::array<char, kDisplayNameCapacity>& name) noexcept;
/** Invalidates the public projection after local account or session-overlay changes. */
void local_changed() noexcept;
/**
 * @return Wrapping change token for the local account's profile, presence and investment state.
 *
 * Compare for equality, not ordering.
 */
[[nodiscard]] std::uint64_t local_generation() noexcept;
/** Ephemeral local native publication; private persistence never stores this report. */
[[nodiscard]] bool publish_local_presence(AccountHandle owner,
                                          const social::NativePresence& value) noexcept;
/** @return The local account's last published native presence. */
[[nodiscard]] social::NativePresence local_presence() noexcept;
/** Only the owner's selected character publication; absent or stale reports return empty. */
[[nodiscard]] social::NativePresence native_presence(AccountHandle owner) noexcept;
/** The self endpoint last published; a joined fireteam's leader descriptor cannot replace it. */
[[nodiscard]] std::size_t
peer_endpoints(AccountHandle owner, std::span<network::PeerPublication::Endpoint> output) noexcept;
/** A disconnected account retains its profile but withdraws its native hosting publication. */
void clear_remote_presence(AccountHandle owner) noexcept;
/** Invalidates projections after a committed public service dependency changes. */
void service_changed(AccountHandle owner) noexcept;
/**
 * @return Wrapping change token for public-cache resets, releases, publications and service
 * changes.
 * Local account mutations also require observing local_generation().
 */
[[nodiscard]] std::uint32_t public_generation() noexcept;

} // namespace sunrise::state::account::profiles
