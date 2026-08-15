/**
 * URL host rewrite for external-server mode. The Client looks up its SignOn hosts outside the
 * hooked resolver entries, so pointing it at another server needs a rewritten URL, not a
 * redirected lookup.
 */

#include <algorithm>

#include "../../../core/settings/settings.h"
#include "route.h"

namespace sunrise::client::hooks::external_server {
namespace {

/** Scheme separator that begins the authority of an absolute URL. */
constexpr std::string_view kSchemeSeparator = "://";

} // namespace

/** @return True while the Client is pointed at a server outside this process. */
bool enabled() noexcept {
    return core::settings::get().client.externalServer.enabled;
}

/** Replaces the host of one absolute URL with the external server's address. */
std::size_t rewrite_host(std::string_view url, std::span<char> output) noexcept {
    const std::size_t scheme = url.find(kSchemeSeparator);
    if (scheme == std::string_view::npos || output.empty()) {
        return 0;
    }
    const std::size_t hostStart = scheme + kSchemeSeparator.size();
    const std::size_t pathStart = url.find('/', hostStart);
    const std::string_view prefix = url.substr(0, hostStart);
    const std::string_view suffix =
        pathStart == std::string_view::npos ? std::string_view{} : url.substr(pathStart);
    const std::string_view host(core::settings::get().client.externalServer.host.data());
    const std::size_t size = prefix.size() + host.size() + suffix.size();
    if (host.empty() || size + 1 > output.size()) {
        return 0;
    }

    auto next = std::copy(prefix.begin(), prefix.end(), output.begin());
    next = std::copy(host.begin(), host.end(), next);
    next = std::copy(suffix.begin(), suffix.end(), next);
    *next = '\0';
    return size;
}

} // namespace sunrise::client::hooks::external_server
