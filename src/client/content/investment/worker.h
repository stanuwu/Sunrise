#pragma once

#include <cstdint>

namespace sunrise::client::content::investment::worker {

/** Allows cooperative investment refresh slices on the caller-owned game thread. */
void activate() noexcept;

/**
 * Runs one due bounded refresh slice on the caller-owned game thread.
 * @param nowMilliseconds Current monotonic process tick in milliseconds.
 */
void service(std::uint64_t nowMilliseconds) noexcept;

/** Stops taking refresh slices and clears the pending overlay. */
void reset() noexcept;

} // namespace sunrise::client::content::investment::worker
