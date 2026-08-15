#pragma once

#include <span>

#include "../../../patterns/registry.h"
#include "../config_getter.h"

namespace sunrise::client::targets::game::config_getter {

/**
 * Derives both config getter thunks.
 * @param image Executable ranges from the main game image.
 * @param output Receives both thunk addresses.
 * @return True when each signature matched exactly once.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept;

/** @param targets Validated config getter table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::config_getter
