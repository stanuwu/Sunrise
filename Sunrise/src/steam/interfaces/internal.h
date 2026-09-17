#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../runtime/runtime.h"

namespace sunrise::steam::interfaces {

/** A quaternion has 4 components. */
inline constexpr std::size_t kQuaternionComponents = 4;
/** A motion vector has 3 components. */
inline constexpr std::size_t kVectorComponents = 3;
/** Steam result code for success. */
inline constexpr int kResultOk = 1;

#pragma pack(push, 1)
struct InputDigitalActionData {
    bool state{};
    bool active{};
};

struct InputAnalogActionData {
    int mode{};
    float x{};
    float y{};
    bool active{};
};

struct InputMotionData {
    float rotation[kQuaternionComponents]{};
    float acceleration[kVectorComponents]{};
    float velocity[kVectorComponents]{};
};
#pragma pack(pop)

struct SteamId {
    std::uint64_t value{};
};

#pragma pack(push, 1)
struct FriendGameInfo {
    std::uint64_t gameId{};
    std::uint32_t gameIp{};
    std::uint16_t gamePort{};
    std::uint16_t queryPort{};
    std::uint64_t lobbySteamId{};
};
struct PersonaStateChange {
    std::uint64_t steamId{};
    int changeFlags{};
};
struct GameRichPresenceJoinRequested {
    std::uint64_t friendSteamId{};
    /** The connect string's own field width in the Steamworks callback struct. */
    char connect[256]{};
};
#pragma pack(pop)
// Sizes the Steamworks callback structs declare; a caller copies each one by its own size.
static_assert(sizeof(FriendGameInfo) == 24);
static_assert(sizeof(PersonaStateChange) == 12);
static_assert(sizeof(GameRichPresenceJoinRequested) == 264);

namespace versions {

/** Exact interface version strings the callers ask for. Only these are answered. */
inline constexpr char kApps[] = "STEAMAPPS_INTERFACE_VERSION008";
inline constexpr char kInput[] = "SteamInput001";
inline constexpr char kUtils[] = "SteamUtils009";
inline constexpr char kFriends[] = "SteamFriends017";
inline constexpr char kUser[] = "SteamUser020";
inline constexpr char kUserStats[] = "STEAMUSERSTATS_INTERFACE_VERSION011";
inline constexpr char kMatchmaking[] = "SteamMatchMaking009";
inline constexpr char kLegacyClient[] = "SteamClient018";
inline constexpr char kSerializedNetworking[] = "SteamNetworkingSocketsSerialized003";
inline constexpr char kHttp[] = "STEAMHTTP_INTERFACE_VERSION003";

} // namespace versions

/** Owns and sets up the Steam interface vtables. */
namespace tables {

/** Sets every interface table up once, on the first interface request. */
void initialize() noexcept;
[[nodiscard]] void* apps() noexcept;
[[nodiscard]] void* input() noexcept;
[[nodiscard]] void* utils() noexcept;
[[nodiscard]] void* friends() noexcept;
[[nodiscard]] void* user() noexcept;
[[nodiscard]] void* user_stats() noexcept;
[[nodiscard]] void* matchmaking() noexcept;
[[nodiscard]] void* client() noexcept;
[[nodiscard]] void* serialized_networking() noexcept;
[[nodiscard]] void* http() noexcept;

} // namespace tables

/** The Steam interface methods installed into the vtable slots. */
namespace methods {

ULONG_PTR unsupported(void*) noexcept;
bool return_true(void*) noexcept;
const char* persona_name(void*) noexcept;
int friend_count(void*, int flags) noexcept;
SteamId* friend_by_index(void*, SteamId* result, int index, int flags) noexcept;
int friend_relationship(void*, std::uint64_t steamId) noexcept;
int friend_persona_state(void*, std::uint64_t steamId) noexcept;
const char* friend_persona_name(void*, std::uint64_t steamId) noexcept;
bool friend_game_played(void*, std::uint64_t steamId, FriendGameInfo* info) noexcept;
/** Captured before callback-pump activation so shutdown can invalidate an entering pass. */
[[nodiscard]] std::uint64_t friends_generation() noexcept;
void service_friends(std::uint64_t expectedGeneration = 0) noexcept;
void service_invites(std::uint64_t expectedGeneration = 0) noexcept;
void reset_friends() noexcept;
const char* language(void*) noexcept;
const char* country(void*) noexcept;
DWORD get_app_id(void*) noexcept;
UserHandle get_user_handle(void*) noexcept;
SteamId* get_steam_id(void*, SteamId* result) noexcept;
bool app_is_installed(void*, DWORD candidate) noexcept;
bool dlc_installed(void*, DWORD) noexcept;
bool current_beta_name(void*, char*, int) noexcept;
std::uint32_t app_install_dir(void*, DWORD, char*, std::uint32_t) noexcept;
const char* launch_query_param(void*, const char*) noexcept;
int app_build_id(void*) noexcept;
int get_dlc_count(void*) noexcept;
bool get_dlc_data(void*, int, DWORD*, bool*, char*, int) noexcept;
bool request_current_stats(void*) noexcept;
bool get_achievement(void*, const char*, bool*) noexcept;
bool set_achievement(void*, const char*) noexcept;
bool store_stats(void*) noexcept;
bool set_rich_presence(void*, const char*, const char*) noexcept;
void clear_rich_presence(void*) noexcept;
const char* friend_rich_presence(void*, std::uint64_t, const char*) noexcept;
int friend_rich_presence_key_count(void*, std::uint64_t) noexcept;
const char* friend_rich_presence_key(void*, std::uint64_t, int) noexcept;
bool invite_user_to_game(void*, std::uint64_t, const char*) noexcept;
std::uint64_t input_handle(void*, const char*) noexcept;
InputDigitalActionData input_digital_data(void*, std::uint64_t, std::uint64_t) noexcept;
InputAnalogActionData input_analog_data(void*, std::uint64_t, std::uint64_t) noexcept;
InputMotionData input_motion_data(void*, std::uint64_t) noexcept;
int filter_text(void*, char*, DWORD, const char*, bool) noexcept;
ApiCall create_lobby(void*, int, int) noexcept;
ApiCall join_lobby(void*, std::uint64_t) noexcept;
void leave_lobby(void*, std::uint64_t) noexcept;
void service_lobbies() noexcept;
bool send_lobby_chat(void*, std::uint64_t, const void*, int) noexcept;
int get_lobby_chat_entry(void*, std::uint64_t, int, std::uint64_t*, void*, int, int*) noexcept;
void* get_generic_interface(void*, UserHandle, PipeHandle, const char*) noexcept;
void* get_client_user(void*, UserHandle, PipeHandle, const char*) noexcept;
void* get_client_utils(void*, PipeHandle, const char*) noexcept;
void* get_client_http(void*, UserHandle, PipeHandle, const char*) noexcept;
int connected_universe(void*) noexcept;
void serialized_send_rendezvous(void*, std::uint64_t, DWORD, const void*, DWORD) noexcept;
void serialized_send_failure(void*, std::uint64_t, DWORD, DWORD, const char*) noexcept;
ApiCall serialized_get_certificate(void*) noexcept;
int serialized_get_network_config(void*, void*, DWORD, const char*) noexcept;
void serialized_cache_relay_ticket(void*, const void*, DWORD) noexcept;
DWORD serialized_relay_ticket_count(void*) noexcept;
int serialized_get_relay_ticket(void*, DWORD, void*, DWORD) noexcept;
void serialized_post_connection_state(void*, const void*, DWORD) noexcept;
ApiCall request_encrypted_app_ticket(void*, const void*, int) noexcept;
bool get_encrypted_app_ticket(void*, void*, int, DWORD*) noexcept;

/** @return The single local Steam identity this shim answers for. */
[[nodiscard]] inline std::uint64_t local_steam_id() noexcept {
    SteamId identity{};
    (void)get_steam_id(nullptr, &identity);
    return identity.value;
}

/** @param text Optional C string from a caller. @return Its view, or an empty view. */
[[nodiscard]] inline std::string_view text_view(const char* text) noexcept {
    return text != nullptr ? std::string_view{text} : std::string_view{};
}

} // namespace methods
} // namespace sunrise::steam::interfaces
