#include <Windows.h>

#include <array>
#include <cstddef>

#include "internal.h"

namespace sunrise::steam::interfaces::tables {
namespace {

/** A Steam interface object. It holds only its vtable pointer. */
struct InterfaceObject {
    FARPROC* methods{};
};

/** STEAMAPPS_INTERFACE_VERSION008 has 30 vtable slots. */
constexpr std::size_t kAppsMethodCount = 30;
/** SteamInput001 has 35 vtable slots. */
constexpr std::size_t kInputMethodCount = 35;
/** SteamUtils009 has 34 vtable slots. */
constexpr std::size_t kUtilsMethodCount = 34;
/** SteamFriends017 has 80 vtable slots. */
constexpr std::size_t kFriendsMethodCount = 80;
/** SteamUser020 has 31 vtable slots. */
constexpr std::size_t kUserMethodCount = 31;
/** STEAMUSERSTATS_INTERFACE_VERSION011 has 48 vtable slots. */
constexpr std::size_t kUserStatsMethodCount = 48;
/** SteamMatchMaking009 has 37 vtable slots. */
constexpr std::size_t kMatchmakingMethodCount = 37;
/** SteamClient018 has 38 vtable slots. */
constexpr std::size_t kClientMethodCount = 38;
/** Serialized networking version 003 has 8 vtable slots. */
constexpr std::size_t kSerializedNetworkingMethodCount = 8;
/** The HTTP placeholder needs one safe vtable slot. */
constexpr std::size_t kHttpMethodCount = 1;

/** Slots used from STEAMAPPS_INTERFACE_VERSION008. */
enum class AppsSlot : std::size_t {
    subscribed = 0,
    currentLanguage = 4,
    availableLanguages = 5,
    subscribedApp = 6,
    dlcInstalled = 7,
    dlcCount = 10,
    dlcData = 11,
    betaName = 15,
    installDir = 18,
    installed = 19,
    launchQueryParam = 21,
    buildId = 23,
};

/** Slots used from SteamInput001. */
enum class InputSlot : std::size_t {
    initialize = 0,
    shutdown = 1,
    actionSetHandle = 4,
    digitalActionHandle = 11,
    digitalActionData = 12,
    analogActionHandle = 14,
    analogActionData = 15,
    motionData = 20,
};

/** Slots used from SteamUtils009. */
enum class UtilsSlot : std::size_t {
    universe = 2,
    country = 4,
    appId = 9,
    ipv6Connectivity = 31,
    filterText = 32,
};

/** Slots used from SteamFriends017. */
enum class FriendsSlot : std::size_t {
    personaName = 0,
    friendCount = 3,
    friendByIndex = 4,
    friendRelationship = 5,
    friendPersonaState = 6,
    friendPersonaName = 7,
    friendGamePlayed = 8,
    setRichPresence = 43,
    clearRichPresence = 44,
    friendRichPresence = 45,
    friendRichPresenceKeyCount = 46,
    friendRichPresenceKeyByIndex = 47,
    inviteUserToGame = 49,
    listenForFriendsMessages = 64,
    replyToFriendMessage = 65,
};

/** Slots used from SteamUser020. */
enum class UserSlot : std::size_t {
    handle = 0,
    loggedOn = 1,
    steamId = 2,
    requestEncryptedAppTicket = 20,
    getEncryptedAppTicket = 21,
};

/** Slots used from STEAMUSERSTATS_INTERFACE_VERSION011. */
enum class UserStatsSlot : std::size_t {
    requestCurrent = 0,
    achievement = 6,
    setAchievement = 7,
    store = 10,
};

/** Slots used from SteamMatchMaking009. */
enum class MatchmakingSlot : std::size_t {
    createLobby = 13,
    joinLobby = 14,
    leaveLobby = 15,
    sendLobbyChat = 26,
    lobbyChatEntry = 27,
};

/** Slots used from SteamClient018. */
enum class ClientSlot : std::size_t {
    user = 5,
    utils = 9,
    generic = 12,
    http = 24,
};

/** The full slot order of SteamNetworkingSocketsSerialized003. */
enum class SerializedSlot : std::size_t {
    rendezvous = 0,
    failure = 1,
    certificate = 2,
    config = 3,
    cacheTicket = 4,
    ticketCount = 5,
    ticket = 6,
    connectionState = 7,
};

/** The exact function-pointer type for each serialized slot. */
template <SerializedSlot Slot> struct SerializedSignature;

template <> struct SerializedSignature<SerializedSlot::rendezvous> {
    using Type = void (*)(void*, std::uint64_t, DWORD, const void*, DWORD) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::failure> {
    using Type = void (*)(void*, std::uint64_t, DWORD, DWORD, const char*) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::certificate> {
    using Type = ApiCall (*)(void*) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::config> {
    using Type = int (*)(void*, void*, DWORD, const char*) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::cacheTicket> {
    using Type = void (*)(void*, const void*, DWORD) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::ticketCount> {
    using Type = DWORD (*)(void*) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::ticket> {
    using Type = int (*)(void*, DWORD, void*, DWORD) noexcept;
};

template <> struct SerializedSignature<SerializedSlot::connectionState> {
    using Type = void (*)(void*, const void*, DWORD) noexcept;
};

std::array<FARPROC, kAppsMethodCount> g_appsMethods{};
std::array<FARPROC, kInputMethodCount> g_inputMethods{};
std::array<FARPROC, kUtilsMethodCount> g_utilsMethods{};
std::array<FARPROC, kFriendsMethodCount> g_friendsMethods{};
std::array<FARPROC, kUserMethodCount> g_userMethods{};
std::array<FARPROC, kUserStatsMethodCount> g_userStatsMethods{};
std::array<FARPROC, kMatchmakingMethodCount> g_matchmakingMethods{};
std::array<FARPROC, kClientMethodCount> g_clientMethods{};
std::array<FARPROC, kSerializedNetworkingMethodCount> g_serializedMethods{};
std::array<FARPROC, kHttpMethodCount> g_httpMethods{};
InterfaceObject g_apps{g_appsMethods.data()};
InterfaceObject g_input{g_inputMethods.data()};
InterfaceObject g_utils{g_utilsMethods.data()};
InterfaceObject g_friends{g_friendsMethods.data()};
InterfaceObject g_user{g_userMethods.data()};
InterfaceObject g_userStats{g_userStatsMethods.data()};
InterfaceObject g_matchmaking{g_matchmakingMethods.data()};
InterfaceObject g_client{g_clientMethods.data()};
InterfaceObject g_serializedNetworking{g_serializedMethods.data()};
InterfaceObject g_http{g_httpMethods.data()};

INIT_ONCE g_interfaceInit{INIT_ONCE_STATIC_INIT};

/** Points every vtable slot at the safe do-nothing method. */
template <std::size_t Count> void fill_unsupported(std::array<FARPROC, Count>& table) noexcept {
    table.fill(reinterpret_cast<FARPROC>(&methods::unsupported));
}

/** Stores one typed method in the untyped vtable slot a typed ABI slot names. */
template <std::size_t Count, typename Slot, typename Function>
void bind(std::array<FARPROC, Count>& table, Slot slot, Function function) noexcept {
    table[static_cast<std::size_t>(slot)] = reinterpret_cast<FARPROC>(function);
}

/** Binds one serialized slot, but only to a function matching that slot's exact type. */
template <SerializedSlot Slot>
void bind_serialized(typename SerializedSignature<Slot>::Type function) noexcept {
    bind(g_serializedMethods, Slot, function);
}

/** Fills the Apps, Input, Utils, Friends, User and UserStats slots the callers consume. */
void bind_common_methods() noexcept {
    bind(g_appsMethods, AppsSlot::subscribed, &methods::return_true);
    bind(g_appsMethods, AppsSlot::currentLanguage, &methods::language);
    bind(g_appsMethods, AppsSlot::availableLanguages, &methods::language);
    bind(g_appsMethods, AppsSlot::subscribedApp, &methods::return_true);
    bind(g_appsMethods, AppsSlot::dlcInstalled, &methods::dlc_installed);
    bind(g_appsMethods, AppsSlot::dlcCount, &methods::get_dlc_count);
    bind(g_appsMethods, AppsSlot::dlcData, &methods::get_dlc_data);
    bind(g_appsMethods, AppsSlot::betaName, &methods::current_beta_name);
    bind(g_appsMethods, AppsSlot::installDir, &methods::app_install_dir);
    bind(g_appsMethods, AppsSlot::installed, &methods::app_is_installed);
    bind(g_appsMethods, AppsSlot::launchQueryParam, &methods::launch_query_param);
    bind(g_appsMethods, AppsSlot::buildId, &methods::app_build_id);

    bind(g_inputMethods, InputSlot::initialize, &methods::return_true);
    bind(g_inputMethods, InputSlot::shutdown, &methods::return_true);
    bind(g_inputMethods, InputSlot::actionSetHandle, &methods::input_handle);
    bind(g_inputMethods, InputSlot::digitalActionHandle, &methods::input_handle);
    bind(g_inputMethods, InputSlot::digitalActionData, &methods::input_digital_data);
    bind(g_inputMethods, InputSlot::analogActionHandle, &methods::input_handle);
    bind(g_inputMethods, InputSlot::analogActionData, &methods::input_analog_data);
    bind(g_inputMethods, InputSlot::motionData, &methods::input_motion_data);

    bind(g_utilsMethods, UtilsSlot::universe, &methods::connected_universe);
    bind(g_utilsMethods, UtilsSlot::country, &methods::country);
    bind(g_utilsMethods, UtilsSlot::appId, &methods::get_app_id);
    bind(g_utilsMethods, UtilsSlot::ipv6Connectivity, &methods::return_true);
    bind(g_utilsMethods, UtilsSlot::filterText, &methods::filter_text);

    bind(g_friendsMethods, FriendsSlot::personaName, &methods::persona_name);
    bind(g_friendsMethods, FriendsSlot::friendCount, &methods::friend_count);
    bind(g_friendsMethods, FriendsSlot::friendByIndex, &methods::friend_by_index);
    bind(g_friendsMethods, FriendsSlot::friendRelationship, &methods::friend_relationship);
    bind(g_friendsMethods, FriendsSlot::friendPersonaState, &methods::friend_persona_state);
    bind(g_friendsMethods, FriendsSlot::friendPersonaName, &methods::friend_persona_name);
    bind(g_friendsMethods, FriendsSlot::friendGamePlayed, &methods::friend_game_played);
    bind(g_friendsMethods, FriendsSlot::setRichPresence, &methods::set_rich_presence);
    bind(g_friendsMethods, FriendsSlot::clearRichPresence, &methods::clear_rich_presence);
    bind(g_friendsMethods, FriendsSlot::friendRichPresence, &methods::friend_rich_presence);
    bind(g_friendsMethods,
         FriendsSlot::friendRichPresenceKeyCount,
         &methods::friend_rich_presence_key_count);
    bind(g_friendsMethods,
         FriendsSlot::friendRichPresenceKeyByIndex,
         &methods::friend_rich_presence_key);
    bind(g_friendsMethods, FriendsSlot::inviteUserToGame, &methods::invite_user_to_game);
    bind(g_friendsMethods, FriendsSlot::listenForFriendsMessages, &methods::return_true);
    bind(g_friendsMethods, FriendsSlot::replyToFriendMessage, &methods::return_true);

    bind(g_userMethods, UserSlot::handle, &methods::get_user_handle);
    bind(g_userMethods, UserSlot::loggedOn, &methods::return_true);
    bind(g_userMethods, UserSlot::steamId, &methods::get_steam_id);
    bind(
        g_userMethods, UserSlot::requestEncryptedAppTicket, &methods::request_encrypted_app_ticket);
    bind(g_userMethods, UserSlot::getEncryptedAppTicket, &methods::get_encrypted_app_ticket);

    bind(g_userStatsMethods, UserStatsSlot::requestCurrent, &methods::request_current_stats);
    bind(g_userStatsMethods, UserStatsSlot::achievement, &methods::get_achievement);
    bind(g_userStatsMethods, UserStatsSlot::setAchievement, &methods::set_achievement);
    bind(g_userStatsMethods, UserStatsSlot::store, &methods::store_stats);
}

/** Fills the Matchmaking, Client and serialized-networking slots the callers consume. */
void bind_networking_methods() noexcept {
    bind(g_matchmakingMethods, MatchmakingSlot::createLobby, &methods::create_lobby);
    bind(g_matchmakingMethods, MatchmakingSlot::joinLobby, &methods::join_lobby);
    bind(g_matchmakingMethods, MatchmakingSlot::leaveLobby, &methods::leave_lobby);
    bind(g_matchmakingMethods, MatchmakingSlot::sendLobbyChat, &methods::send_lobby_chat);
    bind(g_matchmakingMethods, MatchmakingSlot::lobbyChatEntry, &methods::get_lobby_chat_entry);

    bind(g_clientMethods, ClientSlot::user, &methods::get_client_user);
    bind(g_clientMethods, ClientSlot::utils, &methods::get_client_utils);
    bind(g_clientMethods, ClientSlot::generic, &methods::get_generic_interface);
    bind(g_clientMethods, ClientSlot::http, &methods::get_client_http);

    bind_serialized<SerializedSlot::rendezvous>(&methods::serialized_send_rendezvous);
    bind_serialized<SerializedSlot::failure>(&methods::serialized_send_failure);
    bind_serialized<SerializedSlot::certificate>(&methods::serialized_get_certificate);
    bind_serialized<SerializedSlot::config>(&methods::serialized_get_network_config);
    bind_serialized<SerializedSlot::cacheTicket>(&methods::serialized_cache_relay_ticket);
    bind_serialized<SerializedSlot::ticketCount>(&methods::serialized_relay_ticket_count);
    bind_serialized<SerializedSlot::ticket>(&methods::serialized_get_relay_ticket);
    bind_serialized<SerializedSlot::connectionState>(&methods::serialized_post_connection_state);
}

/**
 * Sets up every interface table before any object is published.
 * @return TRUE for InitOnceExecuteOnce.
 */
BOOL CALLBACK initialize_tables(PINIT_ONCE, PVOID, PVOID*) noexcept {
    fill_unsupported(g_appsMethods);
    fill_unsupported(g_inputMethods);
    fill_unsupported(g_utilsMethods);
    fill_unsupported(g_friendsMethods);
    fill_unsupported(g_userMethods);
    fill_unsupported(g_userStatsMethods);
    fill_unsupported(g_matchmakingMethods);
    fill_unsupported(g_clientMethods);
    fill_unsupported(g_serializedMethods);
    fill_unsupported(g_httpMethods);
    bind_common_methods();
    bind_networking_methods();
    return TRUE;
}

} // namespace

/** Sets every interface table up once, on the first interface request. */
void initialize() noexcept {
    (void)InitOnceExecuteOnce(&g_interfaceInit, initialize_tables, nullptr, nullptr);
}

void* apps() noexcept {
    return &g_apps;
}
void* input() noexcept {
    return &g_input;
}
void* utils() noexcept {
    return &g_utils;
}
void* friends() noexcept {
    return &g_friends;
}
void* user() noexcept {
    return &g_user;
}
void* user_stats() noexcept {
    return &g_userStats;
}
void* matchmaking() noexcept {
    return &g_matchmaking;
}
void* client() noexcept {
    return &g_client;
}
void* serialized_networking() noexcept {
    return &g_serializedNetworking;
}
void* http() noexcept {
    return &g_http;
}

} // namespace sunrise::steam::interfaces::tables
