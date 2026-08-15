#pragma once

#include <cstddef>

namespace sunrise::client::targets::game::config_getter {

/** Resolved config getter thunks the content state machine calls at phase 1. */
struct Targets {
    /** Thunk asked for the config URL. */
    std::byte* url{};
    /** Thunk asked for the config token. */
    std::byte* token{};
};

/** Clears the published config getter group. */
void clear() noexcept;

/** @return Process-local config getter group. */
[[nodiscard]] const Targets& get() noexcept;

/** @return True after both thunks are published. */
[[nodiscard]] bool is_resolved() noexcept;

} // namespace sunrise::client::targets::game::config_getter
