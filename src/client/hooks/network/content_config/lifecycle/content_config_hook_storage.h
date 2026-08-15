#pragma once

#include "../gate/content_config_signature_gate.h"
#include "../internal.h"

namespace sunrise::client::hooks::network::content_config::lifecycle {

extern gate::Ownership g_gateOwnership;
extern bool g_transitionActive;

/** @return True when every hook handle is attached. */
[[nodiscard]] bool all_installed() noexcept;

/** @return True when any hook handle is attached. */
[[nodiscard]] bool any_installed() noexcept;

/** @return True when every hook still admits replacement work. */
[[nodiscard]] bool all_accepting() noexcept;

/** Clears lifecycle state. Safe only once no hook and no live call is left. */
void clear_runtime() noexcept;

/**
 * Removes one hook and points admitted calls at the restored target.
 * @param slot Hook handle to remove.
 * @param replacement Replacement entry kept safe while Detours checks the threads.
 * @return True when the hook is gone after the call.
 */
[[nodiscard]] bool detach(HookSlot slot, void* replacement) noexcept;

} // namespace sunrise::client::hooks::network::content_config::lifecycle
