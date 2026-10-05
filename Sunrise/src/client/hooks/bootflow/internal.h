#pragma once

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::bootflow {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

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

/**
 * Attaches the character-select hold, so the sign-in boot step stays on the character-select
 * screen instead of falling through to orbit on its own.
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_character_select_hold() noexcept;

/** Detaches the character-select hold. */
void uninstall_character_select_hold() noexcept;

} // namespace sunrise::client::hooks::bootflow
