#pragma once

#include <cstdint>

namespace sunrise::server::gameplay::physics::host::session {

/**
 * Opens, ticks and closes one physics world per admitted gameplay peer.
 * This is the only caller of the process host. It produces no wire output: the coordinator is
 * bound and reconciled, but nothing encodes a frame until the external entity body is wired.
 * @param now Monotonic tick count in milliseconds.
 */
void service(std::uint64_t now) noexcept;

/** Closes every open world and releases its State context. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::physics::host::session
