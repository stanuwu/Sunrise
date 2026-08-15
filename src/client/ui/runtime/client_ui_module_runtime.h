#pragma once

namespace sunrise::client::ui::runtime {

/** @return True when the Client module owns its Core UI registry slot. */
[[nodiscard]] bool initialize() noexcept;

/** Removes the Client module from the Core UI registry. */
void shutdown() noexcept;

} // namespace sunrise::client::ui::runtime
