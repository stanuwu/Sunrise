#pragma once

#include <string_view>

namespace sunrise::client::hooks::network::content_config {

/** Process-local URL accepted by the ContentConfig GET replacement. */
inline constexpr std::string_view kLocalUrl = "sunrise://local/config";

} // namespace sunrise::client::hooks::network::content_config
