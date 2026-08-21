#pragma once

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bootflow {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * Attaches the character-select hold, which stops the sign-in step auto-selecting.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_character_select_hold() noexcept;

/** Detaches the character-select hold. */
void uninstall_character_select_hold() noexcept;

/**
 * Attaches the profile-setup skip, which skips the startup setup screens.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_profile_setup_skip() noexcept;

/** Detaches the profile-setup skip. */
void uninstall_profile_setup_skip() noexcept;

/**
 * Attaches the orbit slice-set picker, so the sign-in step's map load finds its target.
 * @return True when the picker is found and the detour attaches.
 */
[[nodiscard]] bool install_orbit_slice_set() noexcept;

/** Detaches the orbit slice-set picker. */
void uninstall_orbit_slice_set() noexcept;

[[nodiscard]] bool install_orbit_seed() noexcept;

void uninstall_orbit_seed() noexcept;

/**
 * Attaches the solo composition fix, which clears the count the matchmaking check rejects.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_composition_check() noexcept;

/** Detaches the solo composition fix. */
void uninstall_composition_check() noexcept;

/**
 * Attaches the orbit handoff release, which stops the destination step parking.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_orbit_handoff() noexcept;

/** Detaches the orbit handoff release. */
void uninstall_orbit_handoff() noexcept;

/**
 * Attaches the join-request readiness force, which moves the activity session to status 6.
 * Two of the gate's five terms are client flags with no host input.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_join_request_ready() noexcept;

/** Detaches the join-request readiness force. */
void uninstall_join_request_ready() noexcept;

/**
 * Attaches the owner activity slot force. It pins the participation record to the replicated
 * snapshot at `comp + 496` instead of the local one at `comp + 1256`.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_owner_activity_slot() noexcept;

/** Detaches the owner activity slot force. */
void uninstall_owner_activity_slot() noexcept;

/**
 * Attaches the private-region force, so a public region takes the path a private one takes.
 * A public region otherwise holds its slice-set switch until a public activity host connects.
 * @return True when both targets are found, the call site is unique and the detour attaches.
 */
[[nodiscard]] bool install_region_private() noexcept;

/** Detaches the private-region force. */
void uninstall_region_private() noexcept;

/**
 * Finds the boot-flow step accessor, the only input to the world phase.
 * Nothing is detoured: the accessor is called, so a miss leaves the phase idle.
 * @return True when the target was found.
 */
[[nodiscard]] bool install_world_step() noexcept;

/** Clears the boot-flow step accessor it found. */
void uninstall_world_step() noexcept;

/**
 * Maps the client's own boot-flow step onto the world phase.
 * Runs on the spawn gate poll, which is the only tick the phase is read on.
 */
void observe_world_step() noexcept;

/**
 * Attaches the spawn hold, which puts the player spawn after the world-transition fade is armed.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_spawn_hold() noexcept;

/** Detaches the spawn hold. */
void uninstall_spawn_hold() noexcept;

/**
 * Finds the world-transition fade release and its manager object.
 * Nothing is detoured: both are called, so a miss leaves the feature off, not the client changed.
 * @return True when both targets were found.
 */
[[nodiscard]] bool install_fade_release() noexcept;

/** Clears the fade release it found. */
void uninstall_fade_release() noexcept;

/**
 * Releases the world-transition fade channel.
 * The spawn gate owns the timing. Does nothing unless `client.fade_release` is set.
 */
void release_world_fade() noexcept;

/** Re-arms the one line the release logs, so the next load reports its own. */
void rearm_fade_release() noexcept;

} // namespace sunrise::client::hooks::bootflow
