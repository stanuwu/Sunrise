#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::middleware::bap::activity_message::join_request {

/**
 * Reads the correlation, nonzero session and member key from an activity join request. Bytes
 * after the fixed typed prefix stay caller-owned and are ignored.
 * @param input Complete join-request activity payload.
 * @param request Cleared first. Receives fields only on success.
 * @return True when the 20-byte prefix is there and its session is nonzero.
 */
[[nodiscard]] bool parse_join_request(std::span<const std::byte> input,
                                      JoinRequest& request) noexcept;

} // namespace sunrise::middleware::bap::activity_message::join_request
