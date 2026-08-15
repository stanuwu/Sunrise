#pragma once

namespace sunrise::client::hooks::network::bubble_authority::scope {

/** Enters one decoder-local authority scope on the current thread. */
void enter() noexcept;

/** Leaves one decoder-local authority scope on the current thread. */
void leave() noexcept;

/** @return True while the current thread is inside an admitted decoder call. */
[[nodiscard]] bool active() noexcept;

} // namespace sunrise::client::hooks::network::bubble_authority::scope
