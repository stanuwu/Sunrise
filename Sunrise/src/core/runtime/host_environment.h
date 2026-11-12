#pragma once

#include <cstdint>

namespace sunrise::core::runtime {

/**
 * Reports whether the process runs under Wine instead of Windows.
 * @return True when the loaded ntdll exports Wine's own version entry point.
 */
[[nodiscard]] bool is_wine() noexcept;

/**
 * Confirms a process has stopped without guessing through access failures.
 * @return True only when Windows reports no process or a signaled process object.
 */
[[nodiscard]] bool process_stopped(std::uint32_t processId) noexcept;

} // namespace sunrise::core::runtime
