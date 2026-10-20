/**
 * Opcode 904 acquires a quest or other pursuit from a vendor.
 * Three 16-bit selectors biased by 0x8000, then a 32-bit
 * field biased by 0x80000000 naming the sale row, then one trailing byte that is skipped.
 */

#include "opcode904_codec.h"

#include <cstddef>

#include "../../../encoding/bit_reader.h"
#include "../biased_field.h"

namespace sunrise::middleware::web_service::messages::opcode904 {
namespace {

/** The sale row is a 32-bit signed value. */
constexpr std::uint8_t kSaleIndexWidth = 32;
/** Its bias is the signed 32-bit midpoint, which is the same rule one width up. */
constexpr std::int64_t kSaleIndexBias = 0x80000000;
/** Bits allowed after the sale row. The body ends in one trailing byte; more than that is data. */
constexpr std::size_t kTrailingLimit = 8;

} // namespace

/** Decodes one quest-acquire request body. */
bool parse_request(const Message& message, Request& output) noexcept {
    if (message.opcode != kOpcode) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    Request candidate{};
    if (!read_biased_index(reader, candidate.vendorIndex)
        || !read_biased_index(reader, candidate.slotIndex)
        || !read_biased_index(reader, candidate.third)) {
        return false;
    }
    std::uint64_t saleIndex = 0;
    if (reader.read(kSaleIndexWidth, saleIndex)) {
        if (reader.remaining_bits() > kTrailingLimit) {
            return false;
        }
        candidate.saleIndex =
            static_cast<std::int32_t>(static_cast<std::int64_t>(saleIndex) - kSaleIndexBias);
        candidate.hasSaleIndex = true;
    }
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode904
