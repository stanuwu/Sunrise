#include "../../parser.h"

namespace sunrise::core::settings::parser {

/** Parses the activation gate block on top of the fixed defaults. */
bool Parser::activation_settings(server::activation::Settings& output) noexcept {
    output = {};
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    server::activation::Settings candidate{};
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        bool value = false;
        if (key == "default_client_activation") {
            if (!boolean(value)) {
                return false;
            }
            candidate.defaultClientActivation = value;
        } else if (key == "activity_compatibility_mirror") {
            if (!boolean(value)) {
                return false;
            }
            candidate.activityCompatibilityMirror = value;
        } else if (key == "gameplay_external_body") {
            if (!boolean(value)) {
                return false;
            }
            candidate.gameplayExternalBody = value;
        } else if (key == "server_default_entity") {
            if (!boolean(value)) {
                return false;
            }
            candidate.serverDefaultEntity = value;
        } else if (key == "physics_host_session") {
            if (!boolean(value)) {
                return false;
            }
            candidate.physicsHostSession = value;
        } else if (key == "activity_public_membership") {
            if (!boolean(value)) {
                return false;
            }
            candidate.activityPublicMembership = value;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            // The whole block is taken at once, so a later invalid key cannot leave half of it.
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

} // namespace sunrise::core::settings::parser
