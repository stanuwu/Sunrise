#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::state_refresh {

/** State-refresh requests use activity message type 18. */
inline constexpr std::uint32_t kMessageType = 18;
/** Two big-endian 32-bit fields form the complete request body. */
inline constexpr std::size_t kEncodedSize = 8;

/** Membership revision and logical bubble index requested by the client. */
struct StateRefresh final {
    std::uint32_t membershipRevision{};
    std::int32_t bubbleIndex{};
};

/**
 * Parses the fixed state-refresh request prefix.
 * @param input Activity message body holding the whole 8-byte prefix.
 * @param refresh Cleared first. Filled in only on success.
 * @return True when both big-endian fields are there in full.
 */
[[nodiscard]] bool parse_state_refresh(std::span<const std::byte> input,
                                       StateRefresh& refresh) noexcept;

} // namespace sunrise::middleware::bap::activity_message::state_refresh
