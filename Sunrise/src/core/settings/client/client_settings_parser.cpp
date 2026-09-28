#include <limits>

#include "../parser.h"

namespace sunrise::core::settings::parser {

/** Parses Client-owned configuration over deterministic defaults. */
bool Parser::client_settings(client::Settings& output, bool& endpointConfigured) noexcept {
    if (!consume('{')) {
        return false;
    }
    client::Settings candidate = output;
    bool hasUserInterface = false;
    bool hasExternalServer = false;
    bool hasCustomBootflowTextures = false;
    bool hasSocketMenuRouting = false;
    bool hasRevealLoreBooks = false;
    bool hasServerEndpoint = false;
    bool hasMachineId = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "server_endpoint") {
            auto& endpoint = candidate.serverEndpoint;
            if (hasServerEndpoint
                || !bap_endpoint(endpoint.host, endpoint.address, endpoint.bapPort)) {
                return false;
            }
            hasServerEndpoint = true;
        } else if (key == "machine_id") {
            if (hasMachineId || !unsigned_integer(candidate.machineId)
                || candidate.machineId == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            hasMachineId = true;
        } else if (key == "ui") {
            if (hasUserInterface || !client_ui_settings(candidate.userInterface)) {
                return false;
            }
            hasUserInterface = true;
        } else if (key == "external_server") {
            if (hasExternalServer || !client_external_settings(candidate.externalServer)) {
                return false;
            }
            hasExternalServer = true;
        } else if (key == "custom_bootflow_textures") {
            if (hasCustomBootflowTextures || !boolean(candidate.customBootflowTextures)) {
                return false;
            }
            hasCustomBootflowTextures = true;
        } else if (key == "socket_menu_routing") {
            if (hasSocketMenuRouting || !boolean(candidate.socketMenuRouting)) {
                return false;
            }
            hasSocketMenuRouting = true;
        } else if (key == "reveal_lore_books") {
            if (hasRevealLoreBooks || !boolean(candidate.revealLoreBooks)) {
                return false;
            }
            hasRevealLoreBooks = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            endpointConfigured = hasServerEndpoint;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
