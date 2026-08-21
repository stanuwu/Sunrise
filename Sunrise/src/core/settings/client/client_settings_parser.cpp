#include "../parser.h"

namespace sunrise::core::settings::parser {
namespace {

constexpr std::uint32_t kHashBasis = 0x811C9DC5U;
constexpr std::uint32_t kHashPrime = 0x01000193U;

[[nodiscard]] std::uint32_t orbit_hash(std::string_view name) noexcept {
    if (name.empty() || name.size() > 48) {
        return 0;
    }
    std::uint32_t value = kHashBasis;
    for (char character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        const bool usable = (character >= 'a' && character <= 'z')
                            || (character >= '0' && character <= '9') || character == '_';
        if (!usable) {
            return 0;
        }
        value = (value * kHashPrime) ^ static_cast<std::uint8_t>(character);
    }
    return value;
}

} // namespace

/** Parses Client-owned configuration over deterministic defaults. */
bool Parser::client_settings(client::Settings& output) noexcept {
    if (!consume('{')) {
        return false;
    }
    client::Settings candidate = output;
    bool hasUserInterface = false;
    bool hasExternalServer = false;
    bool hasFadeRelease = false;
    bool hasForceJoinRequestReady = false;
    bool hasRegionPrivate = false;
    bool hasPinReplicatedRecord = false;
    bool hasHoldSpawn = false;
    bool hasSpawnHoldMs = false;
    bool hasOrbitSliceSet = false;
    if (consume('}')) {
        return true;
    }
    for (;;) {
        std::string_view key;
        if (!string(key) || !consume(':')) {
            return false;
        }
        if (key == "ui") {
            if (hasUserInterface || !client_ui_settings(candidate.userInterface)) {
                return false;
            }
            hasUserInterface = true;
        } else if (key == "external_server") {
            if (hasExternalServer || !client_external_settings(candidate.externalServer)) {
                return false;
            }
            hasExternalServer = true;
        } else if (key == "fade_release") {
            if (hasFadeRelease || !boolean(candidate.fadeRelease)) {
                return false;
            }
            hasFadeRelease = true;
        } else if (key == "force_join_request_ready") {
            if (hasForceJoinRequestReady || !boolean(candidate.forceJoinRequestReady)) {
                return false;
            }
            hasForceJoinRequestReady = true;
        } else if (key == "region_private") {
            if (hasRegionPrivate || !boolean(candidate.regionPrivate)) {
                return false;
            }
            hasRegionPrivate = true;
        } else if (key == "pin_replicated_record") {
            if (hasPinReplicatedRecord || !boolean(candidate.pinReplicatedRecord)) {
                return false;
            }
            hasPinReplicatedRecord = true;
        } else if (key == "hold_spawn") {
            if (hasHoldSpawn || !boolean(candidate.holdSpawn)) {
                return false;
            }
            hasHoldSpawn = true;
        } else if (key == "spawn_hold_ms") {
            std::uint64_t value = 0;
            if (hasSpawnHoldMs || !unsigned_integer(value) || value == 0
                || value > client::kMaximumSpawnHoldMs) {
                return false;
            }
            candidate.spawnHoldMs = value;
            hasSpawnHoldMs = true;
        } else if (key == "orbit_slice_set") {
            std::string_view name;
            if (hasOrbitSliceSet || !string(name)) {
                return false;
            }
            if (!name.empty() && orbit_hash(name) == 0) {
                return false;
            }
            candidate.orbitSliceSetHash = orbit_hash(name);
            hasOrbitSliceSet = true;
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

} // namespace sunrise::core::settings::parser
