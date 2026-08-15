#pragma once

#include <string_view>

#include "snapshot.h"

namespace sunrise::core::log::snapshot::internal {

/** Clears retained events before a new logger lifecycle starts. */
void reset() noexcept;

void record(Channel channel, Level level, std::string_view text) noexcept;

} // namespace sunrise::core::log::snapshot::internal
