#pragma once

#include <span>

#include "../../../patterns/registry.h"
#include "../package_path.h"

namespace sunrise::client::targets::game::package_path {

/**
 * Finds the package path builder, the path object opener and the schema callback dispatcher, and
 * reads the layouts.
 * @param image Executable ranges from the main game image.
 * @param output Receives the three entries and the layout operands.
 * @return True when every signature matched once and the operands are consistent.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept;

/** @param targets Validated package path table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::package_path
