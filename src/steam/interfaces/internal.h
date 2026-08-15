#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "../runtime/runtime.h"

namespace sunrise::steam::interfaces {

/** A quaternion has 4 components. */
inline constexpr std::size_t kQuaternionComponents = 4;
/** A motion vector has 3 components. */
inline constexpr std::size_t kVectorComponents = 3;

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

namespace versions {

/** The exact apps interface version the Client asks for. */
inline constexpr char kApps[] = "STEAMAPPS_INTERFACE_VERSION008";
/** The exact input interface version the Client asks for. */
inline constexpr char kInput[] = "SteamInput001";
/** The exact utils interface version the Client asks for. */
inline constexpr char kUtils[] = "SteamUtils009";
/** The exact friends interface version the Client asks for. */
inline constexpr char kFriends[] = "SteamFriends017";
/** The exact user interface version the Client asks for. */
inline constexpr char kUser[] = "SteamUser020";
/** The exact user-stats interface version the Client asks for. */
inline constexpr char kUserStats[] = "STEAMUSERSTATS_INTERFACE_VERSION011";
/** The exact matchmaking interface version the Client asks for. */
inline constexpr char kMatchmaking[] = "SteamMatchMaking009";
/** The exact legacy client interface version the exports ask for. */
inline constexpr char kLegacyClient[] = "SteamClient018";
/** The exact serialized networking interface version the Client asks for. */
inline constexpr char kSerializedNetworking[] = "SteamNetworkingSocketsSerialized003";
/** The exact HTTP interface version the Client asks for. */
inline constexpr char kHttp[] = "STEAMHTTP_INTERFACE_VERSION003";

} // namespace versions

/** Owns and sets up the Steam interface vtables. */
namespace tables {

/** @return True once every interface table is set up. */
[[nodiscard]] bool initialize() noexcept;
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

ULONG_PTR empty(void*) noexcept;
bool return_true(void*) noexcept;
const char* persona_name(void*) noexcept;
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
std::uint64_t input_handle(void*, const char*) noexcept;
InputDigitalActionData input_digital_data(void*, std::uint64_t, std::uint64_t) noexcept;
InputAnalogActionData input_analog_data(void*, std::uint64_t, std::uint64_t) noexcept;
InputMotionData input_motion_data(void*, std::uint64_t) noexcept;
int filter_text(void*, char*, DWORD, const char*, bool) noexcept;
ApiCall create_lobby(void*, int, int) noexcept;
ApiCall join_lobby(void*, std::uint64_t) noexcept;
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

} // namespace methods
} // namespace sunrise::steam::interfaces
