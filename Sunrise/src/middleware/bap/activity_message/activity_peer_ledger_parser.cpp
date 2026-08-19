/**
 * Fixed peer-ledger bodies: reservation release, peer leave, connectivity failure, and a
 * speculative migration proposal. Each one names a peer key that has to resolve inside the bound
 * session before anything acts on it, so every parser returns the key rather than a decision.
 */

#include <bit>

#include "../../encoding/bit_reader.h"
#include "../../encoding/byte_order.h"
#include "peer_ledger.h"

namespace sunrise::middleware::bap::activity_message::peer_ledger {
namespace {

/** The migration scalar is biased by the midpoint of the unsigned 32-bit range. */
constexpr std::uint32_t kMigrationScalarBias = 0x80000000U;

/** Layout shared by the release body and the leave body that extends it. */
struct LedgerLayout final {
    /** The first opaque scalar starts the body. */
    static constexpr std::size_t scalarA = 0;
    /** The second opaque scalar follows the first. */
    static constexpr std::size_t scalarB = scalarA + encoding::kU32Size;
    /** Eight key bytes, low byte first, in the same order the join request writes its member key.
     */
    static constexpr std::size_t peerKey = scalarB + encoding::kU32Size;
    /** Only the leave body carries this trailing scalar. */
    static constexpr std::size_t scalarC = peerKey + encoding::kU64Size;
};

/** Layout of the migration proposal. */
struct MigrationLayout final {
    /** The biased opaque scalar starts the body. */
    static constexpr std::size_t scalar = 0;
    /** Eight key bytes, low byte first, follow the scalar. */
    static constexpr std::size_t peerKey = scalar + encoding::kU32Size;
};

/**
 * Fills the two scalars and the peer key shared by the release and leave bodies.
 * @param input Payload holding at least the shared prefix.
 * @param scalarA Receives the first scalar.
 * @param scalarB Receives the second scalar.
 * @param peerKey Receives the peer key.
 */
void read_shared_prefix(std::span<const std::byte> input,
                        std::uint32_t& scalarA,
                        std::uint32_t& scalarB,
                        std::uint64_t& peerKey) noexcept {
    scalarA = encoding::read_u32_be(input.subspan<LedgerLayout::scalarA, encoding::kU32Size>());
    scalarB = encoding::read_u32_be(input.subspan<LedgerLayout::scalarB, encoding::kU32Size>());
    peerKey = encoding::read_u64_le(input.subspan<LedgerLayout::peerKey, encoding::kU64Size>());
}

} // namespace

/** Parses a reservation release. */
bool parse_release(std::span<const std::byte> input,
                   ReservationRelease& release,
                   std::size_t& consumedBits) noexcept {
    release = {};
    consumedBits = 0;
    if (input.size() < kReleaseSize) {
        return false;
    }
    read_shared_prefix(input, release.scalarA, release.scalarB, release.peerKey);
    consumedBits = kReleaseSize * encoding::kBitsPerByte;
    return true;
}

/** Parses a peer leave notice. */
bool parse_leave(std::span<const std::byte> input,
                 PeerLeave& leave,
                 std::size_t& consumedBits) noexcept {
    leave = {};
    consumedBits = 0;
    if (input.size() < kLeaveSize) {
        return false;
    }
    read_shared_prefix(input, leave.scalarA, leave.scalarB, leave.peerKey);
    leave.scalarC =
        encoding::read_u32_be(input.subspan<LedgerLayout::scalarC, encoding::kU32Size>());
    consumedBits = kLeaveSize * encoding::kBitsPerByte;
    return true;
}

/** Parses a connectivity failure report. */
bool parse_connectivity_failure(std::span<const std::byte> input,
                                ConnectivityFailure& failure,
                                std::size_t& consumedBits) noexcept {
    failure = {};
    consumedBits = 0;
    if (input.size() < kConnectivityFailureSize) {
        return false;
    }
    // The key is raw bits here rather than a byte field, so the reason that follows it is not
    // byte aligned and the whole body has to be walked as a bitstream.
    encoding::bits::Reader reader(input);
    std::uint64_t key = 0;
    std::uint64_t reason = 0;
    if (!reader.read(encoding::kU64Size * encoding::kBitsPerByte, key)
        || !reader.read(kFailureReasonWidth, reason)) {
        return false;
    }
    failure.peerKey = key;
    failure.rawReason = static_cast<std::uint8_t>(reason);
    consumedBits = input.size() * encoding::kBitsPerByte - reader.remaining_bits();
    return true;
}

/** Parses a speculative migration proposal. */
bool parse_migration(std::span<const std::byte> input,
                     MigrationProposal& proposal,
                     std::size_t& consumedBits) noexcept {
    proposal = {};
    consumedBits = 0;
    if (input.size() < kMigrationSize) {
        return false;
    }
    const std::uint32_t raw =
        encoding::read_u32_be(input.subspan<MigrationLayout::scalar, encoding::kU32Size>());
    proposal.scalar = std::bit_cast<std::int32_t>(raw - kMigrationScalarBias);
    proposal.peerKey =
        encoding::read_u64_le(input.subspan<MigrationLayout::peerKey, encoding::kU64Size>());
    consumedBits = kMigrationSize * encoding::kBitsPerByte;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::peer_ledger
