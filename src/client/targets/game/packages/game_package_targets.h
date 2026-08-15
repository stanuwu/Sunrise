#pragma once

#include <span>

#include "../../../patterns/registry.h"
#include "../packages.h"

namespace sunrise::client::targets::game::packages {

/**
 * Derives the package key-table location.
 * @param image Executable ranges from the main game image.
 * @param output Receives the resolved table address.
 * @return True when the signature matches once and the table is inside a scanned range.
 */
[[nodiscard]] bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept;

/** @param targets Validated package table published without failure. */
void publish(const Targets& targets) noexcept;

} // namespace sunrise::client::targets::game::packages
