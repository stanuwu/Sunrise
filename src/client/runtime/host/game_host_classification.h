#pragma once

#include <string_view>

namespace sunrise::client::runtime::host {

/** Policy result for process-local game network readiness. */
enum class NetworkRequirement {
    required,
    notApplicable,
};

/**
 * Classifies an executable path against the supported game host name.
 * @param executablePath Absolute or relative executable path.
 * @return Required for the game host or unknown input; otherwise not applicable.
 */
[[nodiscard]] NetworkRequirement classify(std::wstring_view executablePath) noexcept;

/** @return Network readiness policy for the current process executable. */
[[nodiscard]] NetworkRequirement current_requirement() noexcept;

} // namespace sunrise::client::runtime::host
