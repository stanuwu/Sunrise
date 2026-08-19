#include "member_messages.h"

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::group {

namespace {

namespace bits = encoding::bits;

/** Width of the NetAddr method selector, which leads the address codec. */
constexpr std::uint8_t kMethodWidth = 3;
/** Address bytes a method of 0 through 5 carries. */
constexpr std::size_t kShortAddressBytes = 41;
/** Address bytes a method of 6 or 7 carries. */
constexpr std::size_t kLongAddressBytes = 85;
/** Smallest method that selects the long address form. */
constexpr std::uint64_t kFirstLongMethod = 6;
/** Width of the reserved field a player-add carries after its session id. */
constexpr std::uint8_t kReservedWidth = 1;
/** Width of the player-add sequence. */
constexpr std::uint8_t kSequenceWidth = 32;
/** Width of the player-add kind. */
constexpr std::uint8_t kKindWidth = 2;

} // namespace

/** Reads the session id and address method of a peer-properties message. */
bool read_peer_properties_header(bits::Reader& reader, PeerPropertiesHeader& output) noexcept {
    PeerPropertiesHeader candidate{};
    std::uint64_t method = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kMethodWidth, method)) {
        return false;
    }
    const std::size_t addressBytes =
        method >= kFirstLongMethod ? kLongAddressBytes : kShortAddressBytes;
    if (!bits::skip_raw(reader, addressBytes)) {
        return false;
    }
    candidate.addressMethod = static_cast<std::uint8_t>(method);
    output = candidate;
    return true;
}

/** Reads the identity fields of a player-add message. */
bool read_player_add(bits::Reader& reader, PlayerAddRequest& output) noexcept {
    PlayerAddRequest candidate{};
    std::uint64_t reserved = 0;
    std::uint64_t sequence = 0;
    std::uint64_t kind = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kReservedWidth, reserved)
        || !bits::read_raw_u64(reader, candidate.playerId) || !reader.read(kSequenceWidth, sequence)
        || !reader.read(kKindWidth, kind)) {
        return false;
    }
    // The consumer refuses a set reserved bit.
    if (reserved != 0) {
        return false;
    }
    candidate.sequence = static_cast<std::uint32_t>(sequence);
    candidate.kind = static_cast<std::uint8_t>(kind);
    output = candidate;
    return true;
}

/** Reads a whole player-remove message. */
bool read_player_remove(bits::Reader& reader, PlayerRemoveRequest& output) noexcept {
    PlayerRemoveRequest candidate{};
    std::uint64_t reserved = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !reader.read(kReservedWidth, reserved)) {
        return false;
    }
    // The consumer refuses a set reserved bit, the same as it does on an add.
    if (reserved != 0) {
        return false;
    }
    output = candidate;
    return true;
}

/** Reads the leading fields of a player-properties message. */
bool read_player_properties_header(bits::Reader& reader, PlayerPropertiesRequest& output) noexcept {
    PlayerPropertiesRequest candidate{};
    std::uint64_t reserved = 0;
    std::uint64_t sequence = 0;
    std::uint64_t kind = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kReservedWidth, reserved)
        || !reader.read(kSequenceWidth, sequence) || !reader.read(kKindWidth, kind)) {
        return false;
    }
    if (reserved != 0) {
        return false;
    }
    candidate.sequence = static_cast<std::uint32_t>(sequence);
    candidate.kind = static_cast<std::uint8_t>(kind);
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::gameplay::group
