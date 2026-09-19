#include <limits>

#include "../../state/activity/defaults/activity_defaults_validation.h"
#include "../../state/entitlements/validation.h"
#include "parser.h"

namespace sunrise::core::settings::parser {

/** @param input Complete JSON text, borrowed and never changed. */
Parser::Parser(std::string_view input) noexcept : input_(input) {}

/** Reads the version without interpreting settings from an older schema. */
bool Parser::parse_version(std::uint32_t& output, bool* compact) noexcept {
    output = 0;
    if (compact) {
        *compact = false;
    }
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return at_end();
    }
    bool found = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "version") {
            std::uint64_t value = 0;
            if (found || !unsigned_integer(value)
                || value > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            output = static_cast<std::uint32_t>(value);
            found = true;
        } else {
            whitespace();
            if (compact
                && (key == "persona"
                    || ((key == "server" || key == "host") && position_ < input_.size()
                        && input_[position_] == '"'))) {
                *compact = true;
            }
            if (!skip_value(0)) {
                return false;
            }
        }
        if (consume('}')) {
            return at_end();
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the supported root object and skips unknown top-level values. */
bool Parser::parse_root(Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return at_end();
    }
    bool hasVersion = false;
    bool hasMultiplayer = false;
    bool hasCore = false;
    bool hasClient = false;
    bool hasClientEndpoint = false;
    bool hasServer = false;
    bool hasSteam = false;
    bool hasState = false;
    bool hasPersona = false;
    bool hasMachineId = false;
    bool hasCompleteExoticCatalysts = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "version") {
            std::uint64_t value{};
            if (hasVersion || !unsigned_integer(value)
                || value > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            output.version = static_cast<std::uint32_t>(value);
            hasVersion = true;
        } else if (key == "multiplayer_enabled") {
            if (hasMultiplayer || !boolean(output.multiplayerEnabled)) {
                return false;
            }
            hasMultiplayer = true;
        } else if (key == "complete_exotic_catalysts") {
            if (hasCompleteExoticCatalysts || !boolean(output.completeExoticCatalysts)) {
                return false;
            }
            hasCompleteExoticCatalysts = true;
        } else if (key == "role") {
            std::string_view value;
            if (output.hasConfiguredRole || !string(value)
                || !parse_role(value, output.configuredRole)) {
                return false;
            }
            output.hasConfiguredRole = true;
        } else if (key == "persona") {
            if (hasPersona || !compact_persona(output.steam.user)) {
                return false;
            }
            hasPersona = true;
        } else if (key == "machine_id") {
            if (hasMachineId || !unsigned_integer(output.client.machineId)
                || output.client.machineId == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            hasMachineId = true;
        } else if (key == "core") {
            if (hasCore || !core(output)) {
                return false;
            }
            hasCore = true;
        } else if (key == "client") {
            if (hasClient || !client_settings(output.client, hasClientEndpoint)) {
                return false;
            }
            hasClient = true;
        } else if (key == "host") {
            if (hasServer || !compact_endpoint(output.client.serverEndpoint)
                || output.client.serverEndpoint.address[0] == 0
                // A first octet of 224 or above is multicast or reserved, never a host.
                || output.client.serverEndpoint.address[0] >= 224) {
                return false;
            }
            output.compactHost = true;
            hasServer = true;
        } else if (key == "server") {
            if (hasServer) {
                return false;
            }
            whitespace();
            if (position_ < input_.size() && input_[position_] == '"') {
                if (!compact_endpoint(output.client.serverEndpoint)) {
                    return false;
                }
                output.compactClient = true;
            } else if (!server_settings(output.server)) {
                return false;
            }
            hasServer = true;
        } else if (key == "steam") {
            if (hasSteam || !steam_settings(output.steam)) {
                return false;
            }
            hasSteam = true;
        } else if (key == "state") {
            if (hasState || !state_settings(output)) {
                return false;
            }
            hasState = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            const bool simpleEndpoint = output.compactClient || output.compactHost;
            if (hasPersona && (!simpleEndpoint || hasSteam)) {
                return false;
            }
            if (simpleEndpoint
                && (output.hasConfiguredRole || hasClientEndpoint
                    || output.client.externalServer.enabled)) {
                return false;
            }
            return at_end();
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses the supported Core settings object. */
bool Parser::core(Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    bool hasLogging = false;
    bool hasActivitySdkGeneration = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "logging") {
            if (hasLogging || !logging(output.logging)) {
                return false;
            }
            hasLogging = true;
        } else if (key == "activity_sdk_generation") {
            if (hasActivitySdkGeneration
                || !activity_sdk_generation_settings(output.activitySdkGeneration)) {
                return false;
            }
            hasActivitySdkGeneration = true;
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

/** Parses the activity SDK generation block. Omitted or unknown members keep the defaults. */
bool Parser::activity_sdk_generation_settings(ActivitySdkGenerationSettings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    ActivitySdkGenerationSettings candidate{};
    if (consume('}')) {
        output = candidate;
        return true;
    }
    bool hasLuaDeclarations = false;
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "lua_declarations") {
            if (hasLuaDeclarations || !boolean(candidate.luaDeclarations)) {
                return false;
            }
            hasLuaDeclarations = true;
        } else if (!skip_value(0)) {
            return false;
        }
        if (consume('}')) {
            output = candidate;
            return true;
        }
        if (!consume(',')) {
            return false;
        }
    }
}

/** Parses logging sinks and channel levels. */
bool Parser::logging(log::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "debugger_sink") {
            if (!boolean(output.debuggerSink)) {
                return false;
            }
        } else if (key == "file_sink") {
            if (!boolean(output.fileSink)) {
                return false;
            }
        } else if (key == "levels") {
            if (!levels(output)) {
                return false;
            }
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

/** Parses named channel levels and ignores unknown channels. */
bool Parser::levels(log::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view channelName;
        std::string_view levelName;
        if (!string(channelName) || !consume(':') || !string(levelName)) {
            return false;
        }

        log::Level level{};
        if (!level_value(levelName, level)) {
            return false;
        }
        const std::size_t index = channel_index(channelName);
        if (index < output.levels.size()) {
            output.levels[index] = level;
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

namespace sunrise::core::settings {

/** @return Default Core settings, with the default logging levels. */
Settings defaults() noexcept {
    // Named members, so adding one to Settings cannot silently shift the rest.
    return Settings{
        .version = kSettingsVersion,
        .completeExoticCatalysts = true,
        .logging = log::defaults(),
        .activitySdkGeneration = {},
        .server = server::Settings{},
        .initialActivityDefaults = state::activity::defaults::authored(),
    };
}

/** Parses the supported settings from complete JSON text. */
bool parse(std::string_view json, Settings& output, ParseFailure* failure) noexcept {
    if (failure) {
        *failure = ParseFailure::invalidDocument;
    }
    Settings parsed = defaults();
    parser::Parser parser(json);
    if (!parser.parse_root(parsed)) {
        return false;
    }
    // An endpoint or role must never implicitly grant network access.
    if (!parsed.multiplayerEnabled
        && (parsed.compactClient || parsed.compactHost || parsed.configuredRole == Role::host
            || parsed.configuredRole == Role::client || parsed.server.upstream.enabled
            || parsed.client.externalServer.enabled || parsed.server.bapBind != kLoopbackOctets
            || parsed.server.gameplay.bindAddress != kLoopbackOctets)) {
        if (failure) {
            *failure = ParseFailure::multiplayerOptInRequired;
        }
        return false;
    }
    if (failure) {
        *failure = ParseFailure::none;
    }
    output = parsed;
    return true;
}

} // namespace sunrise::core::settings
