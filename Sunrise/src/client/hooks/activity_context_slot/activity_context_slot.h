#pragma once

namespace sunrise::client::hooks::activity_context_slot {

/**
 * Attaches the opt-in activity context slot policy at the Client's context activation.
 * A mission script that declares `--! sunrise.activity_context_slot = "swapped"` gets slot 1
 * instead of the direct-launch slot 0; every other activity keeps the Client's own slot.
 * @return True when the policy is attached.
 */
bool install() noexcept;

/** Detaches the policy. @return False when the detour could not be removed. */
bool uninstall() noexcept;

} // namespace sunrise::client::hooks::activity_context_slot
