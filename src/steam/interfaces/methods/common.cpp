#include <cstdint>
#include <cstring>

#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

/** Steam language token used when there is no localization service. */
constexpr char kLanguage[] = "english";
/** Steam callback id for receiving the current user stats. */
constexpr int kUserStatsReceivedCallback = 1101;
/** Steam result code for success. */
constexpr int kResultOk = 1;
/**
 * Account id for the single local user. The Client builds its whole account identity from it,
 * so the authored `primary_soid` is tied to this value and the two move together.
 */
constexpr std::uint64_t kLocalSteamId = 0x0110000130AA9EC5ULL;
/** Steam universe value for the public network. */
constexpr int kConnectedUniverse = 1;
/** FNV-1a 64-bit offset basis for stable action handles. */
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
/** FNV-1a 64-bit prime for stable action handles. */
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
/** The user-stats callback is 24 bytes. */
constexpr std::size_t kUserStatsReceivedSize = 24;

/** Steam current-user-stats callback layout. */
struct UserStatsReceived {
    std::uint64_t gameId{};
    int result{};
    DWORD padding{};
    std::uint64_t userId{};
};

static_assert(sizeof(UserStatsReceived) == kUserStatsReceivedSize);

/** @param value Action name. @return FNV-1a handle, or zero for a missing name. */
[[nodiscard]] std::uint64_t hash_name(const char* value) noexcept {
    if (value == nullptr || *value == '\0') {
        return 0;
    }
    std::uint64_t hash = kFnvOffsetBasis;
    while (*value != '\0') {
        hash ^= static_cast<unsigned char>(*value++);
        hash *= kFnvPrime;
    }
    return hash == 0 ? 1 : hash;
}

} // namespace

/** @return Zero. The call is not supported. */
ULONG_PTR empty([[maybe_unused]] void* self) noexcept {
    return 0;
}

/** @return True. This capability holds no state. */
bool return_true([[maybe_unused]] void* self) noexcept {
    return true;
}

/** @return Language token. It lasts for the whole process. */
const char* language([[maybe_unused]] void* self) noexcept {
    return kLanguage;
}

/** @return The stable local user handle. */
UserHandle get_user_handle([[maybe_unused]] void* self) noexcept {
    return user_handle();
}

/**
 * @param result Caller storage for the SteamId.
 * @return The same storage, or null when none was given.
 */
SteamId* get_steam_id([[maybe_unused]] void* self, SteamId* result) noexcept {
    if (result != nullptr) {
        result->value = kLocalSteamId;
    }
    return result;
}

/**
 * Reports every application as installed. A DLC id reported as owned but not installed makes the
 * Client treat its content as missing.
 * @return True for every id.
 */
bool app_is_installed([[maybe_unused]] void* self, [[maybe_unused]] DWORD candidate) noexcept {
    return true;
}

/** @return True when the stats callback is queued. */
bool request_current_stats([[maybe_unused]] void* self) noexcept {
    const UserStatsReceived received{app_id(), kResultOk, 0, 0};
    return queue_callback(kUserStatsReceivedCallback, 0, &received, sizeof(received));
}

/**
 * @param achieved Receives false when storage is given.
 * @return True when the output storage is valid.
 */
bool get_achievement([[maybe_unused]] void* self,
                     [[maybe_unused]] const char* name,
                     bool* achieved) noexcept {
    if (achieved == nullptr) {
        return false;
    }
    *achieved = false;
    return true;
}

/** @return Stable nonzero action handle, or zero for a missing name. */
std::uint64_t input_handle([[maybe_unused]] void* self, const char* name) noexcept {
    return hash_name(name);
}

/** @return Two false flags. The digital action is never active. */
InputDigitalActionData input_digital_data([[maybe_unused]] void* self,
                                          [[maybe_unused]] std::uint64_t controllerHandle,
                                          [[maybe_unused]] std::uint64_t actionHandle) noexcept {
    return {};
}

/** @return A zeroed record. The analog action is never active. */
InputAnalogActionData input_analog_data([[maybe_unused]] void* self,
                                        [[maybe_unused]] std::uint64_t controllerHandle,
                                        [[maybe_unused]] std::uint64_t actionHandle) noexcept {
    return {};
}

/** @return A zeroed motion record. */
InputMotionData input_motion_data([[maybe_unused]] void* self,
                                  [[maybe_unused]] std::uint64_t controllerHandle) noexcept {
    return {};
}

/**
 * Copies the input text through unchanged. Nothing is filtered.
 * @param outputSize Output size, including the null.
 * @return Bytes copied, without the null.
 */
int filter_text([[maybe_unused]] void* self,
                char* output,
                DWORD outputSize,
                const char* input,
                [[maybe_unused]] bool compatibilityFlag) noexcept {
    if (output == nullptr || outputSize == 0) {
        return 0;
    }
    if (input == nullptr) {
        output[0] = '\0';
        return 0;
    }
    DWORD count{};
    while (count + 1 < outputSize && input[count] != '\0') {
        output[count] = input[count];
        ++count;
    }
    output[count] = '\0';
    return static_cast<int>(count);
}

/** @return The serialized networking interface, or null for bad input. */
void* get_generic_interface([[maybe_unused]] void* self,
                            UserHandle user,
                            PipeHandle pipe,
                            const char* version) noexcept {
    if (user != user_handle() || pipe != pipe_handle() || version == nullptr) {
        return nullptr;
    }
    return std::strcmp(version, versions::kSerializedNetworking) == 0
               ? tables::serialized_networking()
               : nullptr;
}

/** @return The user interface, or null for bad input. */
void* get_client_user([[maybe_unused]] void* self,
                      UserHandle user,
                      PipeHandle pipe,
                      const char* version) noexcept {
    return user == user_handle() && pipe == pipe_handle() && version != nullptr
                   && std::strcmp(version, versions::kUser) == 0
               ? tables::user()
               : nullptr;
}

/** @return The utils interface, or null for bad input. */
void* get_client_utils([[maybe_unused]] void* self, PipeHandle pipe, const char* version) noexcept {
    return pipe == pipe_handle() && version != nullptr
                   && std::strcmp(version, versions::kUtils) == 0
               ? tables::utils()
               : nullptr;
}

/** @return The HTTP interface, or null for bad input. */
void* get_client_http([[maybe_unused]] void* self,
                      UserHandle user,
                      PipeHandle pipe,
                      const char* version) noexcept {
    return user == user_handle() && pipe == pipe_handle() && version != nullptr
                   && std::strcmp(version, versions::kHttp) == 0
               ? tables::http()
               : nullptr;
}

/** @return The Steam public-universe id. */
int connected_universe([[maybe_unused]] void* self) noexcept {
    return kConnectedUniverse;
}

} // namespace sunrise::steam::interfaces::methods
