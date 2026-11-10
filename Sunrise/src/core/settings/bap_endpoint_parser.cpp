#include <algorithm>
#include <charconv>
#include <limits>

#include "address_text.h"
#include "parser.h"

namespace sunrise::core::settings::parser {

// One reader serves both endpoint settings, so the two must declare the same field widths.
static_assert(client::server_endpoint::kHostCapacity == server::upstream::kHostCapacity);
static_assert(client::server_endpoint::kAddressOctets == server::upstream::kAddressOctets);

bool Parser::bap_endpoint(
    std::array<char, client::server_endpoint::kHostCapacity>& host,
    std::array<unsigned char, client::server_endpoint::kAddressOctets>& address,
    std::uint16_t& port) noexcept {
    if (!consume('{')) {
        return false;
    }
    auto nextHost = host;
    auto nextAddress = address;
    auto nextPort = port;
    bool hasHost = false;
    bool hasPort = false;
    if (!consume('}')) {
        for (;;) {
            std::string_view key;
            if (!string(key) || !consume(':')) {
                return false;
            }
            if (key == "host") {
                std::string_view value;
                if (hasHost || !string(value) || value.size() >= nextHost.size()
                    || !settings::address::parse_ipv4(value, nextAddress)) {
                    return false;
                }
                nextHost.fill(0);
                std::copy(value.begin(), value.end(), nextHost.begin());
                hasHost = true;
            } else if (key == "bap_port") {
                std::uint64_t value = 0;
                if (hasPort || !unsigned_integer(value) || value == 0
                    || value > (std::numeric_limits<std::uint16_t>::max)()) {
                    return false;
                }
                nextPort = static_cast<std::uint16_t>(value);
                hasPort = true;
            } else if (!skip_value(0)) {
                return false;
            }
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }
    host = nextHost;
    address = nextAddress;
    port = nextPort;
    return true;
}

bool Parser::compact_endpoint(client::server_endpoint::Settings& output) noexcept {
    std::string_view value;
    if (!string(value)) {
        return false;
    }
    const auto separator = value.find(':');
    const auto host = value.substr(0, separator);
    auto candidate = output;
    if (host.size() >= candidate.host.size() || !address::parse_ipv4(host, candidate.address)) {
        return false;
    }
    if (separator != std::string_view::npos) {
        const auto port = value.substr(separator + 1);
        unsigned int parsed = 0;
        const auto result = std::from_chars(port.data(), port.data() + port.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != port.data() + port.size()
            || parsed == 0
            // A port is a 16-bit field and zero names no service.
            || parsed > (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        candidate.bapPort = static_cast<std::uint16_t>(parsed);
    }
    candidate.host.fill(0);
    std::copy(host.begin(), host.end(), candidate.host.begin());
    output = candidate;
    return true;
}

} // namespace sunrise::core::settings::parser
