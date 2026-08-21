#pragma once

#include <string_view>

namespace sunrise::client::hooks::bootflow {

void note_destination(std::string_view packageName) noexcept;

/**
 * Attaches the boot-step fixes that carry sign-in through to character select.
 * Each fix reports its own outcome, so a single miss never disables the others.
 * @return True when every fix attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every boot-step fix. */
void uninstall() noexcept;

/** @return True while at least one boot-step fix is attached. */
[[nodiscard]] bool is_installed() noexcept;

/** Publishes the client's own boot-flow step. Call it per frame, on a game thread. */
void poll_world_step() noexcept;

/**
 * Reports whether the player is in a loaded destination.
 * The step is published by a frame poll, so a tick that stops reads as out of world rather than
 * as the last step for ever.
 * @return True while the published step is `activity:in_world` and fresh.
 */
[[nodiscard]] bool in_world() noexcept;

} // namespace sunrise::client::hooks::bootflow
