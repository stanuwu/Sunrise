#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::core::settings {

/** How this process participates: solo, joining another machine's Server, or hosting one. */
enum class Role : std::uint8_t {
    /** Client and Server in one process, for one player alone. */
    embedded,
    /** Client only; this process's Server is another machine's. */
    client,
    /** Client and Server in one process, also serving other machines. */
    host,
    /** Parse failure sentinel; `configure_role` refuses it. */
    invalid,
};

/**
 * @return True when `text` names a role. `output` is untouched otherwise, and never set to
 * `invalid`.
 */
[[nodiscard]] bool parse_role(std::string_view text, Role& output) noexcept;
/** @return The configured role. Safe to read from any thread. */
[[nodiscard]] Role role() noexcept;
/** Settings select solo, joining client, or playing host before hook activation. */
[[nodiscard]] bool configure_role(Role value, bool specified) noexcept;
/** @return True for every role whose process installs the client hooks: embedded, client, host. */
[[nodiscard]] constexpr bool activates_client_hooks(Role value) noexcept {
    return value == Role::embedded || value == Role::client || value == Role::host;
}
/** @return True when this role's process opens the Server's own listening ports. */
[[nodiscard]] constexpr bool binds_server_ports(Role value) noexcept {
    return value == Role::embedded || value == Role::host;
}

/** Shared session services can run in the playing host's DLL. */
[[nodiscard]] inline bool hosts_session() noexcept {
    return role() == Role::host;
}

} // namespace sunrise::core::settings
