/**
 * The start-new-activity request. The body opens with a selector and the two activity indices the
 * move is between, then three optional identity fields, then a nested selection record whose shape
 * is not recovered. Parsing stops at that record and reports the rest as a bounded tail.
 */

#include <bit>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "../activity_host_manager/request/selection/internal.h"
#include "start_activity.h"

namespace sunrise::middleware::bap::activity_message::start_activity {
namespace {

/**
 * Reads one biased index field.
 * @param reader Reader positioned at the field.
 * @param width Field width in bits.
 * @param value Receives the decoded value.
 * @return True when the field was present.
 */
[[nodiscard]] bool
read_index(encoding::bits::Reader& reader, std::uint8_t width, std::int32_t& value) noexcept {
    std::uint64_t raw = 0;
    if (!reader.read(width, raw)) {
        return false;
    }
    value = static_cast<std::int32_t>(raw) - kIndexBias;
    return true;
}

/**
 * Reads one presence bit and, when it is set, the field behind it.
 * @param reader Reader positioned at the presence bit.
 * @param width Width of the field behind the bit.
 * @param value Receives the raw field value when it is present.
 * @param present Receives whether the field was there.
 * @return True when the presence bit and any field behind it were both available.
 */
[[nodiscard]] bool read_optional(encoding::bits::Reader& reader,
                                 std::uint8_t width,
                                 std::uint64_t& value,
                                 bool& present) noexcept {
    std::uint64_t flag = 0;
    if (!reader.read(kPresenceWidth, flag)) {
        return false;
    }
    present = flag != 0;
    if (!present) {
        value = 0;
        return true;
    }
    return reader.read(width, value);
}

} // namespace

/** Parses a start-new-activity request as far as its recovered grammar reaches. */
bool parse_start_activity(std::span<const std::byte> input,
                          StartActivity& request,
                          std::size_t& consumedBits) noexcept {
    request = {};
    consumedBits = 0;
    encoding::bits::Reader reader(input);
    if (!read_index(reader, kSelectorWidth, request.selector)
        || !read_index(reader, kActivityIndexWidth, request.sourceActivityIndex)
        || !read_index(reader, kActivityIndexWidth, request.destinationActivityIndex)) {
        return false;
    }

    std::uint64_t field = 0;
    if (!read_optional(reader, kElementWidth, field, request.hasElement)) {
        return false;
    }
    request.element =
        request.hasElement ? static_cast<std::int32_t>(field) - kIndexBias : kAbsentIndex;
    if (!read_optional(reader, kIdentityWidth, field, request.hasIdentity)) {
        return false;
    }
    request.identity = std::bit_cast<std::int64_t>(field);
    if (!read_optional(
            reader, kIdentityWidth, request.selectionIdentity, request.hasSelectionIdentity)) {
        return false;
    }

    // Preserve the established partial/framing boundary. The shared continuation is diagnostic:
    // failure leaves the packet accepted to the same prefix as before.
    request.tailBits = static_cast<std::uint32_t>(reader.remaining_bits());
    consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
    auto continuationReader = reader;
    request.hasContinuation = activity_host_manager::request::selection::parse_continuation(
        continuationReader, request.continuation);
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::start_activity
