#pragma once

#include <string_view>

namespace sunrise::izanami::runtime::carrier_probe {

/**
 * Reads one installed destination and reports the slice registries a blank package patch would
 * need to reduce. The probe never changes package or game memory.
 */
void inspect(std::string_view scenarioName, std::string_view mapRootName) noexcept;

} // namespace sunrise::izanami::runtime::carrier_probe
