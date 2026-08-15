#include "activity_client_identity_parser.h"

#include <bit>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::bap::activity_message::client_identity {
namespace {

/** The first signed identity field carries 10 bits with a bias of 1. */
constexpr std::uint32_t kField1Bias = 1;
/** The second signed identity field uses the sign bit as its wire bias. */
constexpr std::uint32_t kField2Bias = 0x80000000U;
/** 8 byte elements encode the member key, low byte first. */
constexpr std::size_t kMemberKeyByteCount = 8;
/** 6 trailing padding bits finish the last schema byte. */
constexpr std::uint8_t kPaddingBitCount = 6;

/**
 * Reads the low-byte-first member-key array.
 * @param reader Bounded reader sitting at identity field zero.
 * @param memberKey Receives the key in host order.
 * @return True when all 8 elements are there.
 */
[[nodiscard]] bool read_member_key(encoding::bits::Reader& reader,
                                   std::uint64_t& memberKey) noexcept {
    memberKey = 0;
    for (std::size_t index = 0; index < kMemberKeyByteCount; ++index) {
        std::uint64_t byte = 0;
        if (!reader.read(8, byte)) {
            return false;
        }
        memberKey |= byte << (index * 8U);
    }
    return true;
}

} // namespace

/** Parses the fixed client-identity prefix into logical signed and identity fields. */
bool parse_client_identity(std::span<const std::byte> input, ClientIdentity& identity) noexcept {
    identity = {};
    // The bound is only what the reads below need. An exact length is the client's
    // business, not a rule to enforce here.
    if (input.size() < kEncodedSize) {
        return false;
    }

    encoding::bits::Reader reader(input);
    ClientIdentity parsed{};
    std::uint64_t field1Wire = 0;
    std::uint64_t field2Wire = 0;
    std::uint64_t padding = 0;
    if (!read_member_key(reader, parsed.memberKey) || !reader.read(10, field1Wire)
        || !reader.read(32, field2Wire) || !reader.read(64, parsed.field3)
        || !reader.read(64, parsed.accountSoid) || !reader.read(64, parsed.field5)
        || !reader.read(64, parsed.field6) || !reader.read(kPaddingBitCount, padding)) {
        return false;
    }
    // The padding value and any trailing bits are the client's business. Reading them is enough.
    (void)padding;

    const auto field1Stored = static_cast<std::uint32_t>(field1Wire) - kField1Bias;
    const auto field2Stored = static_cast<std::uint32_t>(field2Wire) - kField2Bias;
    parsed.field1 = std::bit_cast<std::int32_t>(field1Stored);
    parsed.field2 = std::bit_cast<std::int32_t>(field2Stored);
    identity = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::client_identity
