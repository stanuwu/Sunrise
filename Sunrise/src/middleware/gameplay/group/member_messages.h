#pragma once

#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "native_player_profile.h"

namespace sunrise::middleware::gameplay::group {

/** Registry id a member uses to publish its own peer properties. */
inline constexpr std::uint8_t kPeerPropertiesId = 31;
/** Registry id a member uses to ask the host to add one player. */
inline constexpr std::uint8_t kPlayerAddId = 34;
/** Registry id a member uses to ask the host to remove its player. */
inline constexpr std::uint8_t kPlayerRemoveId = 36;
/** Registry id a member uses to publish a sparse change to its player record. */
inline constexpr std::uint8_t kPlayerPropertiesId = 37;
/** Declared decoded size of a peer-properties message. */
inline constexpr std::uint32_t kPeerPropertiesSize = 408;
/** Declared decoded size of a player-add message. */
inline constexpr std::uint32_t kPlayerAddSize = 288;
/** Declared decoded size of a player-remove message. */
inline constexpr std::uint32_t kPlayerRemoveSize = 12;
/** Declared decoded size of a player-properties message. */
inline constexpr std::uint32_t kPlayerPropertiesSize = 288;

/**
 * Leading fields of a peer-properties message.
 * The 304-byte property block after the address is not decoded.
 */
struct PeerPropertiesHeader {
    std::uint64_t sessionId{};
    /** NetAddr method. 0 through 5 carry 41 address bytes; 6 and 7 carry 85. */
    std::uint8_t addressMethod{};
};

/**
 * Complete native player-add body, including the 232-byte B block and 20-byte tail.
 */
struct PlayerAddRequest {
    std::uint64_t sessionId{};
    std::uint64_t playerId{};
    std::uint32_t sequence{};
    /** Player kind, 0 through 3. */
    std::uint8_t kind{};
    /** The block's account and character soids. A host must republish them or no player is made. */
    PlayerBlockSoids soids{};
    /** All own native profile fields from the same full-mode block. */
    NativePlayerProfile profile{};
};

/**
 * Reads the session id and address method of a peer-properties message.
 * @param reader Reader positioned at the body.
 * @param output Receives the fields.
 * @return True when both were present.
 */
[[nodiscard]] bool read_peer_properties_header(encoding::bits::Reader& reader,
                                               PeerPropertiesHeader& output) noexcept;

/** A player-remove message. It names no player: the identity comes from the bound peer state. */
struct PlayerRemoveRequest {
    std::uint64_t sessionId{};
};

/**
 * Native player-properties message, including its sparse B fields and mandatory tail.
 */
struct PlayerPropertiesRequest {
    std::uint64_t sessionId{};
    std::uint32_t sequence{};
    /** Player kind, 0 through 3. */
    std::uint8_t kind{};
    NativePlayerProfile profile{};
    bool hasBaselineChecksum{};
    std::uint32_t baselineChecksum{};
};

/**
 * Reads the complete player-add message.
 * @param reader Reader positioned at the body.
 * @param output Receives the fields.
 * @return True when every field was present and the reserved bit read zero.
 */
[[nodiscard]] bool read_player_add(encoding::bits::Reader& reader,
                                   PlayerAddRequest& output) noexcept;

/**
 * Reads a whole player-remove message.
 * @param reader Reader positioned at the body.
 * @param output Receives the session id.
 * @return True when both fields were present and the reserved bit read zero.
 */
[[nodiscard]] bool read_player_remove(encoding::bits::Reader& reader,
                                      PlayerRemoveRequest& output) noexcept;

/**
 * Reads the leading fields of a player-properties message.
 * @param reader Reader positioned at the body.
 * @param output Receives the fields.
 * @return True when every field was present and the reserved bit read zero.
 */
[[nodiscard]] bool read_player_properties_header(encoding::bits::Reader& reader,
                                                 PlayerPropertiesRequest& output) noexcept;
/** Reads the complete native sparse publication and its optional baseline checksum. */
[[nodiscard]] bool read_player_properties(encoding::bits::Reader& reader,
                                          PlayerPropertiesRequest& output) noexcept;

} // namespace sunrise::middleware::gameplay::group
