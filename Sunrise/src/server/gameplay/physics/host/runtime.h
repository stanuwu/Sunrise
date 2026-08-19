#pragma once

#include "bubble_host.h"

namespace sunrise::server::gameplay::physics::host::runtime {

/** Creates the process host and dormant entity-state registry. */
[[nodiscard]] bool initialize() noexcept;
/** @return Process host, or null before successful initialization. */
[[nodiscard]] BubbleHost* instance() noexcept;
/** @return Immutable process host, or null before successful initialization. */
[[nodiscard]] const BubbleHost* read_only_instance() noexcept;
/** Releases host-owned worlds before clearing entity-state storage. */
void shutdown() noexcept;

} // namespace sunrise::server::gameplay::physics::host::runtime
