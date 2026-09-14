#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::server::bap::proxy {

enum class Plane : std::uint8_t { local, upstream };

/** Resolves the local owner's live SQLite roots, including newly created characters. */
[[nodiscard]] bool is_local_root(std::uint64_t soid) noexcept;

[[nodiscard]] Plane classify(std::uint16_t service, std::span<const std::byte> body) noexcept;

} // namespace sunrise::server::bap::proxy
