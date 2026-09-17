#pragma once

#include <cstdint>

namespace sunrise::server::bap::proxy::profile_publisher {

/** All operations are serialized by the BAP session lock, including lifecycle reset. */
[[nodiscard]] bool acknowledge(std::uint32_t connectionId,
                               std::uint32_t taskId,
                               std::uint16_t responseService) noexcept;
void abandon(std::uint32_t connectionId,
             std::uint32_t taskId,
             std::uint16_t responseService) noexcept;
void reset() noexcept;
/** Reuses bounded storage; cached encoded bytes support acknowledgement and change detection. */
void service(std::uint64_t now) noexcept;

} // namespace sunrise::server::bap::proxy::profile_publisher
