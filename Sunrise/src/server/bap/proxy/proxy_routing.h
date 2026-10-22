#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap::proxy {

/** Which side of the proxy a request belongs on. */
enum class Plane : std::uint8_t { local, upstream };

/** True when the local account owns this SQLite root; false in a public or remote scope. */
[[nodiscard]] bool is_local_root(std::uint64_t soid) noexcept;

/** Decides whether a request stays on the local shim or crosses to the upstream server. */
[[nodiscard]] Plane classify(std::uint16_t service, std::span<const std::byte> body) noexcept;

} // namespace sunrise::server::bap::proxy
