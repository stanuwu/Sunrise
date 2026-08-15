#include "activity_state_refresh_parser.h"

#include <bit>

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message::state_refresh {
namespace {

/** The bubble field uses the signed 32-bit midpoint as its wire bias. */
constexpr std::uint32_t kBubbleIndexBias = 0x80000000U;

} // namespace

/** Parses the fixed state-refresh request prefix. */
bool parse_state_refresh(std::span<const std::byte> input, StateRefresh& refresh) noexcept {
    refresh = {};
    // The bound is only what the reads below need. An exact length is the client's
    // business, not a rule to enforce here.
    if (input.size() < kEncodedSize) {
        return false;
    }

    StateRefresh parsed{};
    parsed.membershipRevision = encoding::read_u32_be(input.first<encoding::kU32Size>());
    const std::uint32_t bubbleWire =
        encoding::read_u32_be(input.subspan<encoding::kU32Size, encoding::kU32Size>());
    parsed.bubbleIndex = std::bit_cast<std::int32_t>(bubbleWire - kBubbleIndexBias);
    refresh = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::state_refresh
