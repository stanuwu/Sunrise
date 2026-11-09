#pragma once

#include "../../ui/runtime/settings.h"
#include "external/definition.h"
#include "server_endpoint/definition.h"

namespace sunrise::core::settings::client {

/** Opt-in startup exports for Sunrise Studio, written below the Sunrise `exports` directory. */
struct Exports {
    /** Writes the installed package keys to `package_keys.bin`. */
    bool packageKeys{false};
    /** Writes the tag reflection records to `tagdefs-raw.<sha8>.json`. */
    bool tagDefinitions{false};
};

/** Read-only Client settings parsed by Core. */
struct Settings {
    server_endpoint::Settings serverEndpoint;
    std::uint64_t machineId{};
    /** In-game UI visibility and input policy. */
    ui::runtime::Settings userInterface;
    /** Points the Client at a server outside this process. Off answers everything in process. */
    external::Settings externalServer;
    /** Replaces stock bootflow textures that have matching DDS assets embedded in Sunrise. */
    bool customBootflowTextures{true};
    /** Moves the four Arrivals leg mods into the leg plug set, so the leg mod menu lists them. */
    bool socketMenuRouting{false};
    /**
     * Clears the visibility gates on the loaded lore presentation nodes.
     * On by default; a client stand-in until the unlock banks carry every gate the nodes read.
     */
    bool revealLoreBooks{true};
    /** Unlocks the cheat tools. A session with them unlocked saves no progress. */
    bool cheats{false};
    /** Startup exports for Sunrise Studio; all off by default. */
    Exports exports;
};

} // namespace sunrise::core::settings::client
