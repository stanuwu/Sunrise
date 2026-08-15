#pragma once

#include <cstdint>

namespace sunrise::server {

/** Starts the in-process server surface. */
[[nodiscard]] bool initialize() noexcept;

/** Runs one bounded server service slice. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept;

/** Stops the in-process server surface. */
void shutdown() noexcept;

} // namespace sunrise::server
