#pragma once

namespace sunrise::client::hooks::instance_mutex {
[[nodiscard]] bool install(void* gameModule) noexcept;
[[nodiscard]] bool uninstall() noexcept;
void release_once() noexcept;
} // namespace sunrise::client::hooks::instance_mutex
