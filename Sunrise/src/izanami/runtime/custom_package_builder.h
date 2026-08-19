#pragma once

#include <string_view>

namespace sunrise::izanami::runtime::custom_package_builder {

/**
 * Builds a non-loadable staged Pandora patch that redirects large scenery to the baseplate map.
 * The `.izanami-stage` suffix keeps Destiny from registering it until every changed block has
 * round-trip validated and a deliberate installation step has succeeded.
 */
[[nodiscard]] bool stage_map_root(std::string_view rootName) noexcept;

} // namespace sunrise::izanami::runtime::custom_package_builder
