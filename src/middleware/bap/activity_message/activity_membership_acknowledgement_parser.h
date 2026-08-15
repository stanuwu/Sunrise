#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::membership_acknowledgement {

/** Membership acknowledgements use activity message type 38. */
inline constexpr std::uint32_t kMessageType = 38;
/** One big-endian revision forms the complete acknowledgement body. */
inline constexpr std::size_t kEncodedSize = 4;

/** Revision of the last membership snapshot applied by the client. */
struct MembershipAcknowledgement final {
    std::uint32_t membershipRevision{};
};

/**
 * Parses the fixed membership-acknowledgement prefix.
 * @param input Activity message body holding the whole 4-byte prefix.
 * @param acknowledgement Cleared first. Filled in only on success.
 * @return True when the whole big-endian revision is there.
 */
[[nodiscard]] bool
parse_membership_acknowledgement(std::span<const std::byte> input,
                                 MembershipAcknowledgement& acknowledgement) noexcept;

} // namespace sunrise::middleware::bap::activity_message::membership_acknowledgement
