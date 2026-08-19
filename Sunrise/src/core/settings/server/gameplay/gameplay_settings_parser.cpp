#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "../../address_text.h"
#include "../../parser.h"

namespace sunrise::core::settings::parser {
namespace {

namespace gameplay = server::gameplay;

/**
 * Turns one topology token into its enum value.
 * @param name Borrowed JSON token.
 * @param output Receives the topology only for a known token.
 * @return True when the token names a supported topology.
 */
[[nodiscard]] bool topology_value(std::string_view name, gameplay::Topology& output) noexcept {
    if (name == "disabled") {
        output = gameplay::Topology::disabled;
        return true;
    }
    if (name == "embedded") {
        output = gameplay::Topology::embedded;
        return true;
    }
    if (name == "external") {
        output = gameplay::Topology::external;
        return true;
    }
    return false;
}

} // namespace

/** Parses the gameplay endpoint block on top of the fixed defaults. */
bool Parser::gameplay_settings(gameplay::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    gameplay::Settings candidate = output;
    if (consume('}')) {
        return gameplay::valid(candidate);
    }
    bool hasTopology = false;
    bool hasBind = false;
    bool hasAdvertised = false;
    bool hasTransport = false;
    bool hasPort = false;
    bool hasReserve = false;
    bool hasJoinGrant = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "topology") {
            std::string_view name;
            if (hasTopology || !string(name) || !topology_value(name, candidate.topology)) {
                return false;
            }
            hasTopology = true;
        } else if (key == "bind_address") {
            std::string_view text;
            if (hasBind || !string(text) || !address::parse_ipv4(text, candidate.bindAddress)) {
                return false;
            }
            hasBind = true;
        } else if (key == "advertised_address") {
            std::string_view text;
            if (hasAdvertised || !string(text)
                || !address::parse_ipv4(text, candidate.advertisedAddress)) {
                return false;
            }
            hasAdvertised = true;
        } else if (key == "transport_address") {
            std::string_view text;
            if (hasTransport || !string(text)
                || !address::parse_ipv4(text, candidate.transportAddress)) {
                return false;
            }
            hasTransport = true;
        } else if (key == "port") {
            std::uint64_t value = 0;
            if (hasPort || !unsigned_integer(value)
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            candidate.port = static_cast<std::uint16_t>(value);
            hasPort = true;
        } else if (key == "server_reserve_count") {
            std::uint64_t value = 0;
            if (hasReserve || !unsigned_integer(value)
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            candidate.serverReserveCount = static_cast<std::uint16_t>(value);
            hasReserve = true;
        } else if (key == "client_join_grant_count") {
            std::uint64_t value = 0;
            if (hasJoinGrant || !unsigned_integer(value)
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            candidate.clientJoinGrantCount = static_cast<std::uint16_t>(value);
            hasJoinGrant = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            // The block is rejected as a whole so a half-applied topology never binds.
            if (!gameplay::valid(candidate)) {
                return false;
            }
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
