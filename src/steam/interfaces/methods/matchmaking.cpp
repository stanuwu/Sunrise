#include <array>

#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

/** Steam callback id for a finished lobby entry. */
constexpr int kLobbyEnterCallback = 504;
/** Steam callback id for a finished lobby creation. */
constexpr int kLobbyCreatedCallback = 513;
/** Steam result code for success. */
constexpr int kResultOk = 1;
/** Steam lobby-entry response for a successful join. */
constexpr DWORD kLobbyEnterSuccess = 1;
/** Made-up lobby ids use Steam's chat-lobby account-type prefix. */
constexpr std::uint64_t kLobbySteamIdPrefix = 0x0109000000000000ULL;
/** Padding aligns the response field after the one-byte lock flag. */
constexpr std::size_t kLobbyLockPadding = 3;
/** Steam's lobby-entry callback is 24 bytes. */
constexpr std::size_t kLobbyEnterSize = 24;
/** Steam's lobby-created callback is 16 bytes. */
constexpr std::size_t kLobbyCreatedSize = 16;

/** Steam lobby-entry callback payload layout. */
struct LobbyEnter {
    std::uint64_t lobby{};
    DWORD permissions{};
    bool locked{};
    std::array<std::byte, kLobbyLockPadding> padding{};
    DWORD response{};
};

/** Steam lobby-created callback payload layout. */
struct LobbyCreated {
    int result{};
    DWORD padding{};
    std::uint64_t lobby{};
};

static_assert(sizeof(LobbyEnter) == kLobbyEnterSize);
static_assert(sizeof(LobbyCreated) == kLobbyCreatedSize);

} // namespace

/**
 * Makes up a lobby, then queues the created and entered callbacks.
 * @return API call id, or zero when either callback cannot be queued.
 */
ApiCall create_lobby([[maybe_unused]] void* self,
                     [[maybe_unused]] int lobbyType,
                     [[maybe_unused]] int maxMembers) noexcept {
    const ApiCall call = next_api_call();
    const std::uint64_t lobby = kLobbySteamIdPrefix | call;
    const LobbyCreated created{kResultOk, 0, lobby};
    const LobbyEnter entered{lobby, 0, false, {}, kLobbyEnterSuccess};
    if (!queue_callback(kLobbyCreatedCallback, call, &created, sizeof(created))) {
        return 0;
    }
    // Entry follows creation, so a reader sees a valid lobby first.
    if (!queue_callback(kLobbyEnterCallback, 0, &entered, sizeof(entered))) {
        return 0;
    }
    return call;
}

/**
 * Queues a successful entry for an existing nonzero lobby.
 * @return API call id, or zero when the lobby id is zero or the queue is full.
 */
ApiCall join_lobby([[maybe_unused]] void* self, std::uint64_t lobby) noexcept {
    if (lobby == 0) {
        return 0;
    }
    const ApiCall call = next_api_call();
    const LobbyEnter entered{lobby, 0, false, {}, kLobbyEnterSuccess};
    return queue_callback(kLobbyEnterCallback, call, &entered, sizeof(entered)) ? call : 0;
}

/** Drops a lobby chat payload. Nothing is kept. @return True for a size of zero or more. */
bool send_lobby_chat([[maybe_unused]] void* self,
                     [[maybe_unused]] std::uint64_t lobby,
                     [[maybe_unused]] const void* data,
                     int size) noexcept {
    return size >= 0;
}

/** @return Zero. The shim keeps no lobby chat history. */
int get_lobby_chat_entry([[maybe_unused]] void* self,
                         [[maybe_unused]] std::uint64_t lobby,
                         [[maybe_unused]] int messageIndex,
                         [[maybe_unused]] std::uint64_t* sender,
                         [[maybe_unused]] void* data,
                         [[maybe_unused]] int dataCapacity,
                         [[maybe_unused]] int* entryType) noexcept {
    return 0;
}

} // namespace sunrise::steam::interfaces::methods
