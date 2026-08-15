#pragma once

#include <cstddef>

namespace sunrise::client::targets::game::assert_handler {

/** Resolved assert-reporting targets. */
struct Targets {
    /** Slot the assert sites call through when it is non-null. */
    std::byte** slot{};
    /** Handler the game installed, retained so detach can restore it. */
    void* original{};
};

/** Clears the published assert target group. */
void clear() noexcept;

/** @return Process-local assert target group. */
[[nodiscard]] const Targets& get() noexcept;

/** @return True after the assert slot is published. */
[[nodiscard]] bool is_resolved() noexcept;

} // namespace sunrise::client::targets::game::assert_handler
