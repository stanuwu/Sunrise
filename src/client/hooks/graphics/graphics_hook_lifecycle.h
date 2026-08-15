#pragma once

namespace sunrise::client::hooks::graphics {

/** @return True when the whole D3D11 presentation hook surface is active. */
[[nodiscard]] bool install() noexcept;

/** @return True when graphics hooks and every owned UI resource are released. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True when both DXGI method detours are attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::graphics
