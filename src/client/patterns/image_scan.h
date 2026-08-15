#pragma once

#include <cstddef>
#include <span>

#include "registry.h"
#include "signature_text.h"

namespace sunrise::client::patterns {

/**
 * Scans the main image for one signature, independently of the shared registry.
 * A single-hook target belongs here, not in the shared sweep: one miss there kills the group.
 * @param signature Exact or wildcarded bytes.
 * @param name Diagnostic name only.
 * @return The unique match, or null when the image has no match or more than one.
 */
[[nodiscard]] std::byte* scan_main_image_unique(std::span<const PatternByte> signature,
                                                const char* name) noexcept;

/**
 * Decodes a RIP-relative operand into the absolute address it refers to.
 * @param operand Address of the 4-byte signed displacement.
 * @param nextInstruction Address just past the instruction being decoded.
 * @return The address the operand points at.
 */
[[nodiscard]] std::byte* resolve_relative(const std::byte* operand,
                                          const std::byte* nextInstruction) noexcept;

} // namespace sunrise::client::patterns
