#pragma once

#include <string_view>

namespace sunrise::state::build_data::hash_names::overlay {

inline constexpr std::wstring_view kFileSuffix = L"\\bubble_names.txt";

[[nodiscard]] bool load(void* module) noexcept;

} // namespace sunrise::state::build_data::hash_names::overlay
