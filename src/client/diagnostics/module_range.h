#pragma once

#include <Windows.h>

#include <cstdint>

namespace sunrise::client::diagnostics {

/** Half-open address range of one loaded image. */
struct ModuleRange {
    std::uintptr_t base{};
    std::uintptr_t end{};
};

/**
 * Reads one loaded image's address range from its mapped PE headers.
 * @param module Loaded image.
 * @param output Receives the half-open range only on success.
 * @return True when the headers are a valid PE image.
 */
[[nodiscard]] bool module_range(HMODULE module, ModuleRange& output) noexcept;

/**
 * Tests one address against an image range.
 * @param range Candidate image range, or a cleared range that holds nothing.
 * @return True when the address lies inside the image.
 */
[[nodiscard]] bool contains(const ModuleRange& range, std::uintptr_t address) noexcept;

} // namespace sunrise::client::diagnostics
