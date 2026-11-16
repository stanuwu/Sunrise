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

/** The tag heap budget the client reserves on its own, in MiB. */
inline constexpr std::uint32_t kTagHeapStockMib = 474;
/**
 * The tag heap budget Sunrise sets unless the settings say otherwise, in MiB. It gives the tag
 * data of installed mods 550 MiB beyond the client's own budget.
 */
inline constexpr std::uint32_t kTagHeapDefaultMib = 1024;
/**
 * The largest tag heap budget the setting accepts, in MiB. It stays under 2 GiB, where a signed
 * 32-bit offset into the heap would turn negative.
 */
inline constexpr std::uint32_t kTagHeapMaximumMib = 2047;

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
    /**
     * Budget in MiB for the client's tag heap, which holds loaded tag data. Installed mods that
     * load more than the client's 474 MiB leave the heap full: content stops loading and the
     * client asserts `Cortex TAG OOM`. 474 is the client's own budget. The client reserves and
     * commits the whole heap once at startup, so a change needs a restart. The part the client
     * never fills costs commit charge, not physical memory.
     */
    std::uint32_t tagHeapMib{kTagHeapDefaultMib};
};

} // namespace sunrise::core::settings::client
