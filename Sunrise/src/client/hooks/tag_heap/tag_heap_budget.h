#pragma once

namespace sunrise::client::hooks::tag_heap {

/**
 * Raises the client's tag heap budget to `client.tag_heap_mib`; the client's own 474 MiB changes
 * nothing. The client creates the heap once at its full size and places its next allocation
 * directly after it, so the budget has to change at Steam init, before the heap exists.
 * @return True when the setting is the client's own budget or the budget was raised.
 */
[[nodiscard]] bool install() noexcept;

} // namespace sunrise::client::hooks::tag_heap
