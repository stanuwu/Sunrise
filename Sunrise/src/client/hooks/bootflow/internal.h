#pragma once

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bootflow {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * How far one boot-step fix got before the group's shared attach.
 * Staging and attaching are split so the whole group holds one detour transaction, not one each.
 * A fix that staged is always published, with a detached handle when the attach did not happen.
 */
enum class StageResult : unsigned char {
    /** The target is missing. The fix reported that itself and staged nothing. */
    unavailable,
    /** An earlier install already attached this fix, so there is nothing to stage. */
    attached,
    /** The spec is filled and the fix wants attaching. */
    staged,
};

/**
 * Stages the character-select hold, which stops the sign-in step auto-selecting.
 * @param spec Receives the target and replacement.
 * @return staged when the target was found, unavailable on a miss.
 */
[[nodiscard]] StageResult stage_character_select_hold(hooking::detour::Spec& spec) noexcept;

/** Takes the character-select hold's attached handle, or a detached one. */
void publish_character_select_hold(const hooking::detour::Handle& handle) noexcept;

/** Detaches the character-select hold. */
void uninstall_character_select_hold() noexcept;

/**
 * Stages the orbit slice-set picker, so the sign-in step's map load finds its target.
 * @param spec Receives the target and replacement.
 * @return staged when the picker was found, unavailable on a miss.
 */
[[nodiscard]] StageResult stage_orbit_slice_set(hooking::detour::Spec& spec) noexcept;

/** Takes the orbit slice-set picker's attached handle, or a detached one. */
void publish_orbit_slice_set(const hooking::detour::Handle& handle) noexcept;

/** Detaches the orbit slice-set picker. */
void uninstall_orbit_slice_set() noexcept;

/**
 * Stages the orbit handoff release, which stops the destination step parking.
 * @param spec Receives the target and replacement.
 * @return staged when the target was found, unavailable on a miss.
 */
[[nodiscard]] StageResult stage_orbit_handoff(hooking::detour::Spec& spec) noexcept;

/** Takes the orbit handoff release's attached handle, or a detached one. */
void publish_orbit_handoff(const hooking::detour::Handle& handle) noexcept;

/** Detaches the orbit handoff release. */
void uninstall_orbit_handoff() noexcept;

/**
 * Finds the boot-flow step accessor behind `in_world`.
 * Nothing is detoured: the accessor is called, so a miss reads as out of world.
 * @return True when the target was found.
 */
[[nodiscard]] bool install_world_step() noexcept;

/** Clears the boot-flow step accessor. */
void uninstall_world_step() noexcept;

/**
 * Attaches the read-only lifetime gate probe, a diagnostic on step 38's joinability gate.
 * @return True when the reader and its helpers were found and the detour attached.
 */
[[nodiscard]] bool install_lifetime_gate_probe() noexcept;

/** Detaches the lifetime gate probe. */
void uninstall_lifetime_gate_probe() noexcept;

} // namespace sunrise::client::hooks::bootflow
