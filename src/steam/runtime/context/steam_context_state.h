#pragma once

namespace sunrise::steam::context {

/** Invalidates every caller-owned Steam context table. */
void advance_generation() noexcept;

} // namespace sunrise::steam::context
