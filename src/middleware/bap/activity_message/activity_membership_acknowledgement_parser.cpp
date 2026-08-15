#include "activity_membership_acknowledgement_parser.h"

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message::membership_acknowledgement {

/** Parses the fixed membership-acknowledgement prefix. */
bool parse_membership_acknowledgement(std::span<const std::byte> input,
                                      MembershipAcknowledgement& acknowledgement) noexcept {
    acknowledgement = {};
    // The bound is only what the reads below need. An exact length is the client's
    // business, not a rule to enforce here.
    if (input.size() < kEncodedSize) {
        return false;
    }

    acknowledgement.membershipRevision = encoding::read_u32_be(input.first<kEncodedSize>());
    return true;
}

} // namespace
  // sunrise::middleware::bap::activity_message::membership_acknowledgement
  // namespace membership_acknowledgement
