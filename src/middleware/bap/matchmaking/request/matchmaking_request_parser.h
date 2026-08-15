#pragma once

#include <cstddef>
#include <span>

#include "../definition.h"

namespace sunrise::middleware::bap::matchmaking::request {

/**
 * Parses one service-42 protobuf body. It keeps no reference to the storage.
 * @param input Decrypted request body. The returned descriptor span borrows from it.
 * @return Request fields, or a request with no kind when any scope it reads is malformed.
 */
[[nodiscard]] Request parse(std::span<const std::byte> input) noexcept;

} // namespace sunrise::middleware::bap::matchmaking::request
