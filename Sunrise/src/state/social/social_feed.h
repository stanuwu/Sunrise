#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../network/peer_routes.h"
#include "lobby_chat.h"

namespace sunrise::state::social::feed {

inline constexpr std::size_t kNameCapacity = 27;
inline constexpr std::size_t kRowCapacity = core::network_capacity::kPlayers + 1;
inline constexpr std::size_t kInviteCapacity = 8;
inline constexpr std::size_t kConnectCapacity = 256;
/** The one version byte both messages open with. A mismatch is refused, never guessed at. */
inline constexpr std::uint8_t kVersion = 6;

/** Request service the shim -- never the game client -- issues on its own BAP link. */
inline constexpr std::uint16_t kSyncRequest = 0x8001;
inline constexpr std::uint16_t kFeedResponse = 0x8002;
/**
 * Host-initiated notice that this account's publication advanced; the guest then asks.
 * Like the two above it, a shim-internal service id the game client never issues or receives, and
 * never a retail id: the decoded catalogue tops out at 307.
 */
inline constexpr std::uint16_t kPublicationNotice = 0x8003;
/** One publication counter. The notice carries nothing else, so it cannot answer anything. */
inline constexpr std::size_t kNoticeBodySize = 8;

/** One invite in transit, in the wire's own terms. */
struct WireInvite {
    std::uint64_t sequence{};
    std::uint64_t targetSoid{};
    std::uint64_t inviterSoid{};
    std::array<char, kConnectCapacity> connect{};
};

struct WireRow {
    std::uint64_t primarySoid{};
    std::uint64_t steamId{};
    std::array<char, kNameCapacity> personaName{};
};

struct Sync {
    lobby::Request lobby{};
    /** Process epoch and delivery acknowledgements; identity comes from the authenticated link. */
    std::uint64_t epoch{};
    std::uint64_t acceptedThrough{};
    std::uint64_t receivedThrough{};
    std::array<WireInvite, kInviteCapacity> invites{};
    std::size_t inviteCount{};
};

struct Feed {
    lobby::Reply lobby{};
    std::uint64_t epoch{};
    std::uint64_t acceptedThrough{};
    /** Delivery token assigned by the network route; the guest asks again past this value. */
    std::uint64_t publication{};
    /** The host's own invite-receipt high-water mark, which confirms the guest's last receipt. */
    std::uint64_t receivedThrough{};
    std::array<WireRow, kRowCapacity> rows{};
    std::size_t rowCount{};
    std::array<WireInvite, kInviteCapacity> invites{};
    std::size_t inviteCount{};
    std::array<network::peer_routes::Endpoint, network::peer_routes::kCapacity> routes{};
    std::size_t routeCount{};
};

[[nodiscard]] bool
encode_sync(const Sync& sync, std::span<std::byte> output, std::size_t& written) noexcept;

[[nodiscard]] bool decode_sync(std::span<const std::byte> body, Sync& sync) noexcept;

[[nodiscard]] bool
encode_feed(const Feed& value, std::span<std::byte> output, std::size_t& written) noexcept;

[[nodiscard]] bool decode_feed(std::span<const std::byte> body, Feed& value) noexcept;

[[nodiscard]] bool encode_notice(std::uint64_t publication,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept;

[[nodiscard]] bool decode_notice(std::span<const std::byte> body,
                                 std::uint64_t& publication) noexcept;

[[nodiscard]] constexpr std::size_t max_body_size() noexcept {

    constexpr std::size_t inviteBytes = kInviteCapacity * (8 + 8 + 8 + 2 + kConnectCapacity);
    constexpr std::size_t syncBytes = 1 + 24 + 1 + inviteBytes;
    constexpr std::size_t feedBytes =
        1 + 32 + 1 + kRowCapacity * (8 + 8 + 1 + kNameCapacity) + 1 + inviteBytes;
    constexpr std::size_t lobbyBytes =
        34 + lobby::kLobbyCapacity * 8 + lobby::kBatchCapacity * (26 + lobby::kPayloadCapacity);
    return (syncBytes > feedBytes ? syncBytes : feedBytes) + lobbyBytes + 1
           + network::peer_routes::kCapacity * 6;
}

} // namespace sunrise::state::social::feed
