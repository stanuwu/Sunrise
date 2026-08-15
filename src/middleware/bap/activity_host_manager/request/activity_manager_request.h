#pragma once

#include <cstddef>
#include <span>

#include "../definition.h"

namespace sunrise::middleware::bap::activity_host_manager::request {

/**
 * Checks one service-6 body and borrows only its declared protobuf bytes. The borrowed bytes
 * can hold credentials and must not outlive the input.
 * @param input Complete decrypted request body.
 * @param request Cleared first. Receives a borrowed view only on success.
 * @return True when the fixed envelope and the declared protobuf length are valid.
 */
[[nodiscard]] bool parse_request(std::span<const std::byte> input, Request& request) noexcept;

} // namespace sunrise::middleware::bap::activity_host_manager::request
