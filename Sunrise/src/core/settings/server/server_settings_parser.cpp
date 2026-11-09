#include <limits>

#include "../../../state/entitlements/validation.h"
#include "../address_text.h"
#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Checks the shared service settings object. */
bool Parser::server_settings(server::Settings& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    bool hasBapPort = false;
    bool hasBapBind = false;
    bool hasGameplay = false;
    bool hasActivation = false;
    bool hasUpstream = false;
    bool hasMaxPlayers = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "max_players") {
            std::uint64_t value{};
            if (hasMaxPlayers || !unsigned_integer(value) || !value
                || value > network_capacity::kPlayers) {
                return false;
            }
            output.maxPlayers = static_cast<std::size_t>(value);
            hasMaxPlayers = true;
        } else if (key == "upstream") {
            auto& upstream = output.upstream;
            if (hasUpstream || !bap_endpoint(upstream.host, upstream.address, upstream.bapPort)) {
                return false;
            }
            upstream.enabled = true;
            hasUpstream = true;
        } else if (key == "bap_bind") {
            std::string_view value;
            if (hasBapBind || !string(value) || !address::parse_ipv4(value, output.bapBind)) {
                return false;
            }
            hasBapBind = true;
        } else if (key == "bap_port") {
            std::uint64_t value = 0;
            if (hasBapPort || !unsigned_integer(value) || value == 0
                || value > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            output.bapPort = static_cast<std::uint16_t>(value);
            hasBapPort = true;
        } else if (key == "gameplay") {
            if (hasGameplay || !gameplay_settings(output.gameplay)) {
                return false;
            }
            hasGameplay = true;
        } else if (key == "activation") {
            if (hasActivation || !activation_settings(output.activation)) {
                return false;
            }
            hasActivation = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
