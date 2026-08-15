#include "client_authoritative_data.h"

namespace sunrise::middleware::bap::activity_message::client_authoritative_data {
namespace {

/** All optional schema fields use 1-bit presence markers. */
constexpr std::uint8_t kPresenceBitWidth = 1;
/** Spawn and teleport state fields use 3 bits with a bias of 1. */
constexpr std::uint8_t kStateBitWidth = 3;
/** Teleport slice-set indices use 10 bits with a bias of 1. */
constexpr std::uint8_t kSliceSetBitWidth = 10;
/** Logical signed fields subtract 1 from their unsigned wire value. */
constexpr std::int32_t kSignedFieldBias = 1;
/** At most 7 zero bits may pad the last meaningful field. */
constexpr std::size_t kMaximumPaddingBitCount = 7;

/**
 * Reads the complete C9 spawn snapshot.
 * @param reader Reader sitting at the required state field.
 * @param update Candidate result that receives the snapshot.
 * @return True when all three required fields fit.
 */
[[nodiscard]] bool read_spawn(encoding::bits::Reader& reader,
                              ClientAuthoritativeData& update) noexcept {
    std::uint64_t stateWire = 0;
    std::uint64_t opaqueByte = 0;
    SpawnState spawn{};
    if (!reader.read(kStateBitWidth, stateWire) || !reader.read(8, opaqueByte)
        || !reader.read(64, spawn.opaqueValue)) {
        return false;
    }
    spawn.state = static_cast<std::int8_t>(static_cast<std::int32_t>(stateWire) - kSignedFieldBias);
    spawn.opaqueByte = static_cast<std::uint8_t>(opaqueByte);
    update.spawn = spawn;
    update.hasSpawn = true;
    return true;
}

/**
 * Reads the reflected C0 teleport fields and skips the unreflected last scalar.
 * @param reader Reader sitting at the required state field.
 * @param update Candidate result that receives the reflected snapshot.
 * @return True when the complete C0 record fits.
 */
[[nodiscard]] bool read_teleport(encoding::bits::Reader& reader,
                                 ClientAuthoritativeData& update) noexcept {
    std::uint64_t stateWire = 0;
    std::uint64_t token = 0;
    std::uint64_t sliceSetWire = 0;
    std::uint64_t sliceSetHash = 0;
    TeleportState teleport{};
    if (!reader.read(kStateBitWidth, stateWire) || !reader.read(8, token)
        || !reader.read(kSliceSetBitWidth, sliceSetWire) || !reader.read(32, sliceSetHash)
        || !reader.skip(64)) {
        return false;
    }
    teleport.state =
        static_cast<std::int8_t>(static_cast<std::int32_t>(stateWire) - kSignedFieldBias);
    teleport.token = static_cast<std::uint8_t>(token);
    teleport.sliceSetIndex = static_cast<std::int32_t>(sliceSetWire) - kSignedFieldBias;
    teleport.sliceSetHash = static_cast<std::uint32_t>(sliceSetHash);
    update.teleport = teleport;
    update.hasTeleport = true;
    return true;
}

/**
 * The reader must end inside its last byte, with only zero padding left.
 * @param reader Reader sitting after the whole schema walk.
 * @return True when no extra byte and no nonzero padding is left.
 */
[[nodiscard]] bool finish(encoding::bits::Reader& reader) noexcept {
    const std::size_t remaining = reader.remaining_bits();
    if (remaining > kMaximumPaddingBitCount) {
        return false;
    }
    std::uint64_t padding = 0;
    return reader.read(static_cast<std::uint8_t>(remaining), padding) && padding == 0
           && reader.remaining_bits() == 0;
}

} // namespace

/** Parses one sparse authoritative delta and keeps only the reflected typed state. */
bool parse_client_authoritative_data(std::span<const std::byte> input,
                                     ClientAuthoritativeData& update) noexcept {
    update = {};
    if (input.size() < kMinimumEncodedSize || input.size() > kMaximumEncodedSize) {
        return false;
    }

    encoding::bits::Reader reader(input);
    std::uint64_t rootWire = 0;
    ClientAuthoritativeData parsed{};
    if (!reader.read(kPresenceBitWidth, rootWire)) {
        return false;
    }
    if (rootWire == 0) {
        return finish(reader);
    }

    bool present = false;
    const bool walked =
        read_presence(reader, present) && (!present || skip_opaque_root_branch(reader))
        && read_presence(reader, present) && (!present || read_transition_branch(reader, parsed))
        && read_presence(reader, present) && (!present || read_spawn(reader, parsed))
        && read_presence(reader, present) && (!present || read_teleport(reader, parsed))
        && finish(reader);
    if (!walked) {
        return false;
    }
    update = parsed;
    return true;
}

} // namespace
  // sunrise::middleware::bap::activity_message::client_authoritative_data
  // client_authoritative_data
