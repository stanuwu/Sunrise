#include "activity_patch_epoch_parser.h"

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::activity_message::patch_epoch {
namespace {

/** The second fixed epoch field follows one unsigned 64-bit value. */
constexpr std::size_t kSecondFieldOffset = encoding::kU64Size;

} // namespace

/** Parses the fixed 2-field big-endian patch-epoch prefix. */
bool parse_patch_epoch(std::span<const std::byte> input, PatchEpoch& epoch) noexcept {
    epoch = {};
    // The bound is only what the reads below need. An exact length is the client's business.
    if (input.size() < kEncodedSize) {
        return false;
    }

    PatchEpoch parsed{};
    parsed.first = encoding::read_u64_be(input.first<encoding::kU64Size>());
    parsed.second = encoding::read_u64_be(input.subspan<kSecondFieldOffset, encoding::kU64Size>());
    epoch = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::patch_epoch
