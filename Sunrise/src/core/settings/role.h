#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::core::settings {

enum class Role : std::uint8_t { embedded, client, host, invalid };

[[nodiscard]] bool parse_role(std::string_view text, Role& output) noexcept;
[[nodiscard]] Role role() noexcept;
/** Settings select solo, joining client, or playing host before hook activation. */
[[nodiscard]] bool configure_role(Role value, bool specified) noexcept;
[[nodiscard]] constexpr bool activates_client_hooks(Role value) noexcept {
    return value == Role::embedded || value == Role::client || value == Role::host;
}
[[nodiscard]] constexpr bool binds_server_ports(Role value) noexcept {
    return value == Role::embedded || value == Role::host;
}

/** Shared session services can run in the playing host's DLL. */
[[nodiscard]] inline bool hosts_session() noexcept {
    return role() == Role::host;
}

} // namespace sunrise::core::settings
