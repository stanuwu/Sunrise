#include <array>
#include <cstring>
#include <span>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/crypto/random_bytes.h"
#include "../../../state/social/lobby_chat.h"
#include "../../../state/steam/steam_state.h"
#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

namespace lobby_state = sunrise::state::steam;
namespace lobby_chat = sunrise::state::social::lobby;

/** Steam callback id for a finished lobby entry. */
constexpr int kLobbyEnterCallback = 504;
/** Steam callback id for a finished lobby creation. */
constexpr int kLobbyCreatedCallback = 513;
/** Steam callback id for a received lobby chat message. */
constexpr int kLobbyChatMsgCallback = 507;
/** Steam chat entry type for an ordinary chat message. */
constexpr std::uint8_t kChatEntryTypeMessage = 1;
/** Padding aligns the chat id after the one-byte entry type. */
constexpr std::size_t kChatTypePadding = 3;
/** Steam's lobby chat callback is 24 bytes. */
constexpr std::size_t kLobbyChatMsgSize = 24;
/** Steam lobby-entry response for a successful join. */
constexpr DWORD kLobbyEnterSuccess = 1;
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

/** Steam lobby chat callback payload layout. */
struct LobbyChatMsg {
    std::uint64_t lobby{};
    std::uint64_t user{};
    std::uint8_t entryType{};
    std::array<std::byte, kChatTypePadding> padding{};
    std::uint32_t chatId{};
};

static_assert(sizeof(LobbyEnter) == kLobbyEnterSize);
static_assert(sizeof(LobbyCreated) == kLobbyCreatedSize);
static_assert(sizeof(LobbyChatMsg) == kLobbyChatMsgSize);

} // namespace

/**
 * Creates a platform lobby and atomically queues its created and entered callbacks.
 * @return API
 * call id, or zero when either callback cannot be queued.
 */
ApiCall create_lobby([[maybe_unused]] void* self,
                     [[maybe_unused]] int lobbyType,
                     [[maybe_unused]] int maxMembers) noexcept {
    const ApiCall call = next_api_call();
    std::uint64_t entropy{};
    if (!middleware::crypto::random::fill(std::as_writable_bytes(std::span(&entropy, 1)))) {
        return 0;
    }
    const auto lobby = lobby_chat::kLobbyPrefix | (entropy & lobby_chat::kLobbyRandomMask);
    if (!lobby_chat::valid_id(lobby) || lobby_chat::contains(lobby) || !lobby_chat::join(lobby)) {
        return 0;
    }
    const LobbyCreated created{kResultOk, 0, lobby};
    const LobbyEnter entered{lobby, 0, false, {}, kLobbyEnterSuccess};
    const std::array deliveries{
        CallbackDelivery{kLobbyCreatedCallback, call, &created, sizeof created},
        CallbackDelivery{kLobbyEnterCallback, 0, &entered, sizeof entered}};
    if (!queue_callbacks(deliveries)) {
        lobby_chat::leave(lobby);
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
    // Native activity joins also use an opaque hosted-session ID here. Keep their existing
    // completion contract; only actual Steam lobby IDs enter the chat membership table.
    const bool joiningChat = lobby_chat::valid_id(lobby) && !lobby_chat::contains(lobby);
    if (joiningChat && !lobby_chat::join(lobby)) {
        return 0;
    }
    const ApiCall call = next_api_call();
    const LobbyEnter entered{lobby, 0, false, {}, kLobbyEnterSuccess};
    if (queue_callback(kLobbyEnterCallback, call, &entered, sizeof(entered))) {
        return call;
    }
    if (joiningChat) {
        lobby_chat::leave(lobby);
    }
    return 0;
}

void leave_lobby([[maybe_unused]] void* self, std::uint64_t lobby) noexcept {
    lobby_chat::leave(lobby);
}

/**
 * Retains one chat message and tells the local member it arrived.
 * @param lobby Destination lobby named by the Client.
 * @param data Exact submitted body, which the Client builds at 0x410 bytes.
 * @param size Submitted byte count.
 * @return True only once the record is retained and its callback is queued.
 */
bool send_lobby_chat([[maybe_unused]] void* self,
                     std::uint64_t lobby,
                     const void* data,
                     int size) noexcept {
    if (core::settings::multiplayer()) {
        return data != nullptr && size > 0
               && lobby_chat::send(
                   lobby, {static_cast<const std::byte*>(data), static_cast<std::size_t>(size)});
    }
    const std::uint64_t sender = local_steam_id();
    std::uint32_t chatId = 0;
    const bool retained =
        data != nullptr && size > 0
        && lobby_state::retain_chat_message(
            lobby,
            sender,
            std::span{static_cast<const std::byte*>(data), static_cast<std::size_t>(size)},
            chatId);
    if (!retained) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=steam stage=lobby_chat_send result=refuse");
        return false;
    }
    const LobbyChatMsg message{lobby, sender, kChatEntryTypeMessage, {}, chatId};
    if (!queue_callback(kLobbyChatMsgCallback, 0, &message, sizeof(message))) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=steam stage=lobby_chat_send result=queue_full");
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::debug,
                     "ev=steam stage=lobby_chat_send result=ok");
    return true;
}

/**
 * Serves one retained chat message back to its reader.
 * @param messageIndex Chat id the callback handed the Client.
 * @param sender Optional storage for the member the message is attributed to.
 * @param data Caller body storage, which must hold the whole retained record.
 * @param entryType Optional storage for the Steam chat entry type.
 * @return Retained body size, or zero when no record matches or it does not fit.
 */
int get_lobby_chat_entry([[maybe_unused]] void* self,
                         std::uint64_t lobby,
                         int messageIndex,
                         std::uint64_t* sender,
                         void* data,
                         int dataCapacity,
                         int* entryType) noexcept {
    if (data == nullptr || dataCapacity <= 0 || messageIndex < 0) {
        return 0;
    }
    if (core::settings::multiplayer()) {
        const auto size = lobby_chat::read(
            lobby,
            messageIndex,
            {static_cast<std::byte*>(data), static_cast<std::size_t>(dataCapacity)},
            sender);
        if (size != 0 && entryType) {
            *entryType = kChatEntryTypeMessage;
        }
        return size;
    }
    lobby_state::ChatRecord record{};
    if (!lobby_state::chat_message(lobby, static_cast<std::uint32_t>(messageIndex), record)
        || record.size > static_cast<std::uint32_t>(dataCapacity)) {
        return 0;
    }
    std::memcpy(data, record.body.data(), record.size);
    if (sender != nullptr) {
        *sender = record.sender;
    }
    if (entryType != nullptr) {
        *entryType = kChatEntryTypeMessage;
    }
    return static_cast<int>(record.size);
}

/** Receipt advances only after the actual platform callback can be queued. */
void service_lobbies() noexcept {
    if (!core::settings::multiplayer()) {
        return;
    }
    lobby_chat::Message message{};
    for (std::size_t i = 0; i < lobby_chat::kQueueCapacity && lobby_chat::pending(message); ++i) {
        const LobbyChatMsg event{message.lobby,
                                 message.sender,
                                 kChatEntryTypeMessage,
                                 {},
                                 static_cast<std::uint32_t>(message.sequence)};
        if (!queue_callback(kLobbyChatMsgCallback, 0, &event, sizeof event)) {
            break;
        }
        lobby_chat::delivered(message.sequence);
    }
}

} // namespace sunrise::steam::interfaces::methods
