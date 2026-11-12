#pragma once

#include <cstdint>

namespace sunrise::server::studio {

/**
 * Answers each typed investment request once, on the server thread, in the slice that copies it.
 * Studio creates the mapping, and until it exists this looks for it once a second.
 * @param now Monotonic tick count in milliseconds.
 */
void service_investment(std::uint64_t now) noexcept;

/** Disconnects from Studio's investment mapping. bap::shutdown releases the retained receipts. */
void shutdown_investment() noexcept;

} // namespace sunrise::server::studio
