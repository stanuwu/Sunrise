#pragma once

#include <span>

#include "../../../patterns/registry.h"
#include "../assert_handler.h"

namespace sunrise::client::targets::game::assert_handler {

/**
 * Derives the assert handler slot from the setter copies and the assert sites that read it.
 * @param image Executable ranges from the main game image.
 * @param output Receives the slot and the handler currently installed in it.
 * @return True when one candidate wins the vote by the required margin.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept;

/** @param targets Validated assert table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::assert_handler
