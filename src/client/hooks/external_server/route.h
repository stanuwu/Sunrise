#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace sunrise::client::hooks::external_server {

/** @return True while the Client is pointed at a server outside this process. */
[[nodiscard]] bool enabled() noexcept;

/**
 * Replaces the host of one absolute URL with the external server's address.
 * The address is numeric, so the rewritten URL needs no name lookup.
 * @param url Borrowed absolute URL.
 * @param output Caller storage receiving the rewritten URL and its trailing null.
 * @return Rewritten length without the null, or zero when it has no host or does not fit.
 */
[[nodiscard]] std::size_t rewrite_host(std::string_view url, std::span<char> output) noexcept;

/**
 * Attaches the `curl_easy_setopt` guard, which unpins TLS and rewrites request URLs.
 * Installs nothing while the external-server switch is off.
 * @return True when the guard is installed, or when the switch leaves it out.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the setopt guard. */
void uninstall() noexcept;

/** @return True while the setopt guard is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::external_server
