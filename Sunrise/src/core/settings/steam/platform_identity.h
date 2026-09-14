#pragma once

#include <cstdint>

namespace sunrise::core::settings::steam::platform_identity {

[[nodiscard]] bool load_or_create(std::uint64_t& token) noexcept;

}
