#include "../../../../encoding/bit_reader.h"
#include "../../../../encoding/byte_order.h"
#include "internal.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
namespace {

using encoding::bits::Reader;

/**
 * Fixed-width fields ahead of the descriptor inside protobuf field two. Only the leading session
 * token varies in size. Everything after it is fixed, so the descriptor sits at one offset once
 * the token length is known.
 */
struct SnapshotPrefix final {
    /** The session token's element count leads the record. */
    static constexpr std::uint8_t tokenCountWidth = 10;
    /** Each token element is one byte. */
    static constexpr std::size_t tokenElementWidth = 8;
    /** Four fixed byte arrays follow the token, in bits. */
    static constexpr std::size_t fixedArrayBits = 2048 + 2048 + 8192 + 256;
    /** Three trailing scalars of the first branch, in bits. */
    static constexpr std::size_t branchScalarBits = 4 + 1 + 8;
    /** Three root scalars ahead of the descriptor, in bits. */
    static constexpr std::size_t rootScalarBits = 4 + 32 + 2;
};

/**
 * Size of the session-token array. A larger count is a misread, not a token. Going on would put
 * the descriptor at the wrong offset and give a wrong destination that still looks right.
 */
constexpr std::uint64_t kTokenCapacity = 568;

} // namespace

/** Decodes the second copy of the descriptor, which rides inside protobuf field two. */
bool parse_field_two(std::span<const std::byte> input,
                     ActivityManagerSelection& selection) noexcept {
    Reader reader(input);
    const std::size_t streamBits = input.size() * encoding::kBitsPerByte;
    std::uint64_t tokenCount = 0;
    if (!reader.read(SnapshotPrefix::tokenCountWidth, tokenCount) || tokenCount > kTokenCapacity
        || !reader.skip(static_cast<std::size_t>(tokenCount) * SnapshotPrefix::tokenElementWidth)
        || !reader.skip(SnapshotPrefix::fixedArrayBits)
        || !reader.skip(SnapshotPrefix::branchScalarBits)
        || !reader.skip(SnapshotPrefix::rootScalarBits)) {
        return false;
    }

    ActivityManagerSelection parsed{};
    const std::size_t descriptorStart = streamBits - reader.remaining_bits();
    if (!parse(reader, parsed)) {
        return false;
    }
    (void)copy_bits(input,
                    descriptorStart,
                    streamBits - reader.remaining_bits() - descriptorStart,
                    parsed.descriptorBits,
                    parsed.descriptorBitLength);
    selection = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
