#include "activity_entity_slot_request_parser.h"

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message::entity_slot_request {
namespace {

/** The wire value is biased by the midpoint of the unsigned 32-bit range. */
constexpr std::int64_t kSignedValueBias = 0x80000000LL;

} // namespace

/** Decodes the biased signed value from the entity-slot request prefix. */
bool parse_entity_slot_request(std::span<const std::byte> input, std::int32_t& value) noexcept {
    value = {};
    // The bound is only what the reads below need. An exact length is the client's
    // business, not a rule to enforce here.
    if (input.size() < kEncodedSize) {
        return false;
    }

    const std::uint32_t raw =
        encoding::read_u32_be(std::span<const std::byte, kEncodedSize>{input});
    const std::int64_t decoded = static_cast<std::int64_t>(raw) - kSignedValueBias;
    value = static_cast<std::int32_t>(decoded);
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::entity_slot_request
