#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "studio_command_protocol.h"

namespace sunrise::server::studio::tag_reload {

/** One answered command. */
struct Reply final {
    command_wire::Status status{};
    /** Tags Sunrise applied itself. */
    std::uint32_t applied{};
    /** Build-data domains the server thread drops so they are read again. */
    std::uint32_t domains{};
    std::array<char, command_wire::kMessageBytes> message{};
};

/**
 * Serves one reloadTags, patchTags or queryResident request on a worker thread: reads its
 * manifest, reloads the mod packages, and writes one result per tag into the manifest.
 * @return False when the previous request's worker still runs.
 */
[[nodiscard]] bool
start(std::uint32_t editorPid, std::uint32_t request, command_wire::Kind kind) noexcept;

/** @return True with the reply once the worker finished this request; another one's is dropped. */
[[nodiscard]] bool take(std::uint32_t request, Reply& reply) noexcept;

/** Waits for a running worker. Runs at shutdown. */
void stop() noexcept;

/**
 * Sets the forced destination to the package, bubble and slice set the player is in now, so the
 * next launch from orbit lands there again. A named bubble is not supported yet.
 * @param bubble The command's message: empty for the current bubble.
 */
[[nodiscard]] Reply reload_bubble(std::string_view bubble) noexcept;

} // namespace sunrise::server::studio::tag_reload
