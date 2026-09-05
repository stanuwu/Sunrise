#pragma once

#include <string_view>

#include "layout.h"

namespace sunrise::core::ui::layout::internal {

/** @return True when the calling thread has the Core-owned context current. */
[[nodiscard]] bool context_is_current() noexcept;

void reset_workspace() noexcept;

void select_module(std::string_view stableId) noexcept;

} // namespace sunrise::core::ui::layout::internal
