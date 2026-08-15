#pragma once

#include <cstddef>
#include <cstdint>

#include "../coordinator/network_call_coordinator.h"

namespace sunrise::client::hooks::network::http {

/** @return Per-thread storage that stands in for a null worker result pointer. */
[[nodiscard]] void* discard_result() noexcept;

/** @param wrapper Game-owned wrapper. @return The value that marks the request handled. */
[[nodiscard]] std::int64_t block_request(void* wrapper) noexcept;

/**
 * Sets the fields the outer HTTP worker copies into the result prefix.
 * @param wrapper Game-owned wrapper.
 * @param responseSize Downloaded size in bytes.
 * @param httpStatus Final HTTP status.
 */
void publish_completion(void* wrapper,
                        std::uint32_t responseSize,
                        std::uint32_t httpStatus) noexcept;

/**
 * Routes one game-owned HTTP descriptor without calling the native transport.
 * @param lease Admission and consumer state held by the replacement.
 * @param wrapper Game-owned HTTP wrapper whose result getters feed the worker.
 * @param descriptor Writable 640-byte request descriptor owned by the worker.
 * @return The value that makes the worker skip the external HTTP operation.
 */
[[nodiscard]] std::int64_t route_descriptor(const coordinator::CallLease& lease,
                                            void* wrapper,
                                            std::byte* descriptor) noexcept;

} // namespace sunrise::client::hooks::network::http
