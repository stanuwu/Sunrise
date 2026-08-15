#include "internal.h"

namespace sunrise::steam::interfaces::tables {
namespace {

/** SteamMatchMaking009 has 37 vtable slots. */
constexpr std::size_t kMatchmakingMethodCount = 37;
/** SteamClient018 has 38 vtable slots. */
constexpr std::size_t kClientMethodCount = 38;
/** Serialized networking version 003 has 8 vtable slots. */
constexpr std::size_t kSerializedNetworkingMethodCount = 8;
/** The HTTP placeholder needs one safe vtable slot. */
constexpr std::size_t kHttpMethodCount = 1;

/** Slots used from SteamMatchMaking009. */
enum class MatchmakingSlot : std::size_t {
    createLobby = 13,
    joinLobby = 14,
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

std::array<FARPROC, kMatchmakingMethodCount> g_matchmakingMethods{};
std::array<FARPROC, kClientMethodCount> g_clientMethods{};
std::array<FARPROC, kSerializedNetworkingMethodCount> g_serializedMethods{};
std::array<FARPROC, kHttpMethodCount> g_httpMethods{};
InterfaceObject g_matchmaking{g_matchmakingMethods.data()};
InterfaceObject g_client{g_clientMethods.data()};
InterfaceObject g_serializedNetworking{g_serializedMethods.data()};
InterfaceObject g_http{g_httpMethods.data()};

/** Stores one serialized method, but only when it matches that slot's type. */
template <SerializedSlot Slot>
void set_serialized_method(typename SerializedSignature<Slot>::Type function) noexcept {
    set_method(g_serializedMethods[index(Slot)], function);
}

} // namespace

/** Sets up the networking tables and fills the slots they use. */
void initialize_networking() noexcept {
    fill_empty(g_matchmakingMethods);
    fill_empty(g_clientMethods);
    fill_empty(g_serializedMethods);
    fill_empty(g_httpMethods);

    set_method(g_matchmakingMethods[index(MatchmakingSlot::createLobby)], &methods::create_lobby);
    set_method(g_matchmakingMethods[index(MatchmakingSlot::joinLobby)], &methods::join_lobby);
    set_method(g_matchmakingMethods[index(MatchmakingSlot::sendLobbyChat)],
               &methods::send_lobby_chat);
    set_method(g_matchmakingMethods[index(MatchmakingSlot::lobbyChatEntry)],
               &methods::get_lobby_chat_entry);

    set_method(g_clientMethods[index(ClientSlot::user)], &methods::get_client_user);
    set_method(g_clientMethods[index(ClientSlot::utils)], &methods::get_client_utils);
    set_method(g_clientMethods[index(ClientSlot::generic)], &methods::get_generic_interface);
    set_method(g_clientMethods[index(ClientSlot::http)], &methods::get_client_http);

    set_serialized_method<SerializedSlot::rendezvous>(&methods::serialized_send_rendezvous);
    set_serialized_method<SerializedSlot::failure>(&methods::serialized_send_failure);
    set_serialized_method<SerializedSlot::certificate>(&methods::serialized_get_certificate);
    set_serialized_method<SerializedSlot::config>(&methods::serialized_get_network_config);
    set_serialized_method<SerializedSlot::cacheTicket>(&methods::serialized_cache_relay_ticket);
    set_serialized_method<SerializedSlot::ticketCount>(&methods::serialized_relay_ticket_count);
    set_serialized_method<SerializedSlot::ticket>(&methods::serialized_get_relay_ticket);
    set_serialized_method<SerializedSlot::connectionState>(
        &methods::serialized_post_connection_state);
}

/** @return The matchmaking interface. */
void* matchmaking() noexcept {
    return &g_matchmaking;
}
/** @return The legacy client interface. */
void* client() noexcept {
    return &g_client;
}
/** @return The serialized networking interface. */
void* serialized_networking() noexcept {
    return &g_serializedNetworking;
}
/** @return The HTTP interface. */
void* http() noexcept {
    return &g_http;
}

} // namespace sunrise::steam::interfaces::tables
