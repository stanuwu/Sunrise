#pragma once

#include <cstdint>

namespace sunrise::server::bap::proxy::profile_publisher {

/** All operations are serialized by the BAP session lock, including lifecycle reset. */
[[nodiscard]] bool acknowledge(std::uint32_t connectionId,
                               std::uint32_t taskId,
                               std::uint16_t responseService) noexcept;
/** Clears the outstanding task only if it is still this one; otherwise a no-op. */
void abandon(std::uint32_t connectionId,
             std::uint32_t taskId,
             std::uint16_t responseService) noexcept;
/** Refuses this image until its local generation or selected connection changes. */
void reject(std::uint32_t connectionId,
            std::uint32_t taskId,
            std::uint16_t responseService) noexcept;
/** Clears publication state so service() retries a fresh image when its inputs/link are ready. */
void reset() noexcept;
/** Publishes changes without a time gate; retains prepared bytes across output pressure. */
void service() noexcept;

} // namespace sunrise::server::bap::proxy::profile_publisher
