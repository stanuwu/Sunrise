#pragma once

#include <cstddef>

namespace sunrise::client::targets::game::retail_log {

/** Unowned main-image entry points used only by retail-log capture. */
struct Targets {
    std::byte* enqueue{};
    std::byte* setCategoryVerbosity{};
};

/** Clears the published retail-log target group. */
void clear() noexcept;

/** @return Process-local retail-log target group. */
[[nodiscard]] const Targets& get() noexcept;

/** @return True after both retail-log targets are published. */
[[nodiscard]] bool is_resolved() noexcept;

} // namespace sunrise::client::targets::game::retail_log
