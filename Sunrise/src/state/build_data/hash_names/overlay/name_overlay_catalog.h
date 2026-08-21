#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../definition.h"

namespace sunrise::state::build_data::hash_names::overlay {

void clear() noexcept;

[[nodiscard]] bool replace(std::span<const Name> names) noexcept;

[[nodiscard]] bool find(std::uint32_t hash, Name& name) noexcept;

[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::hash_names::overlay
