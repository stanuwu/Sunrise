#pragma once

#include <cstdint>

namespace sunrise::server::transport {

/** Starts the nonblocking loopback BAP listener. @return True when the listener is bound. */
[[nodiscard]] bool initialize() noexcept;

/** Runs one bounded listener slice on the caller thread. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept;

/** Closes every socket owned by the listener. */
void shutdown() noexcept;

} // namespace sunrise::server::transport
