#include "steam_context_state.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstring>

#include "../../interfaces/steam_interface_factory.h"
#include "../internal.h"
#include "../runtime.h"

namespace sunrise::steam {
namespace {

/** One stable Steam user handle names the local user. */
constexpr UserHandle kUserHandle = 1;
/** Steam's process-global accessors use the reserved zero user handle. */
constexpr UserHandle kGlobalUserHandle = 0;
/** The shim has one stable Steam pipe handle for its in-process connection. */
constexpr PipeHandle kPipeHandle = 1;
/** Zero is Steam's invalid async API call id. */
constexpr ApiCall kFirstApiCall = 1;
/** The first generation forces the tables to be set up on first use. */
constexpr std::uintptr_t kFirstContextGeneration = 1;
/** SteamNetworkingSockets uses only this legacy client interface. */
constexpr char kNetworkingClientVersion[] = "SteamClient018";

enum class ContextField : std::size_t {
    initializer = 0, // Function that fills the interface field.
    generation = 1,  // Pointer-sized token that marks the table stale.
    interface = 2,   // Cached context interface pointer.
};

SRWLOCK g_contextLock{SRWLOCK_INIT};
std::atomic_uintptr_t g_contextGeneration{kFirstContextGeneration};
std::atomic<DWORD> g_appId{};
std::atomic<ApiCall> g_nextApiCall{kFirstApiCall};

} // namespace

/** @param appId Application id the Steam shim reports. */
void set_app_id(DWORD appId) noexcept {
    g_appId.store(appId, std::memory_order_release);
}

/** @return Current application id. */
DWORD app_id() noexcept {
    return g_appId.load(std::memory_order_acquire);
}

/** @return Next nonzero async API call id. */
ApiCall next_api_call() noexcept {
    ApiCall call = g_nextApiCall.fetch_add(1, std::memory_order_relaxed);
    if (call != 0) {
        return call;
    }
    // The counter can wrap to Steam's reserved zero id. Skip it once.
    call = g_nextApiCall.fetch_add(1, std::memory_order_relaxed);
    return call == 0 ? kFirstApiCall : call;
}

/** @return The shim's single valid user handle. */
UserHandle user_handle() noexcept {
    return kUserHandle;
}

/** @return The shim's single valid pipe handle. */
PipeHandle pipe_handle() noexcept {
    return kPipeHandle;
}

/** Sets up a Steam internal context table in caller storage. */
void* context_init(void* data) noexcept {
    if (data == nullptr) {
        return nullptr;
    }
    auto** fields = static_cast<void**>(data);
    const auto initializerIndex = static_cast<std::size_t>(ContextField::initializer);
    const auto generationIndex = static_cast<std::size_t>(ContextField::generation);
    const auto interfaceIndex = static_cast<std::size_t>(ContextField::interface);
    const auto generation = g_contextGeneration.load(std::memory_order_acquire);
    std::uintptr_t storedGeneration{};
    std::memcpy(&storedGeneration, &fields[generationIndex], sizeof(storedGeneration));

    AcquireSRWLockExclusive(&g_contextLock);
    if (storedGeneration != generation) {
        fields[interfaceIndex] = nullptr;
        const auto initializer = reinterpret_cast<void (*)(void*)>(fields[initializerIndex]);
        if (initializer != nullptr) {
            initializer(&fields[interfaceIndex]);
        }
        // The generation is written after setup, so no reader can accept stale storage.
        std::memcpy(&fields[generationIndex], &generation, sizeof(generation));
    }
    void* result = &fields[interfaceIndex];
    ReleaseSRWLockExclusive(&g_contextLock);
    return result;
}

/** Creates one supported Steam interface. */
void* create_interface(const char* version, const void* callerAddress) noexcept {
    void* const interface = interfaces::create(version);
    if (interface != nullptr && std::strcmp(version, kNetworkingClientVersion) == 0) {
        runtime::activate_platform_once(callerAddress);
    }
    return interface;
}

/** Returns one supported user interface for a valid user handle. */
void* find_or_create_user_interface(UserHandle user, const char* version) noexcept {
    if (user == kGlobalUserHandle) {
        return interfaces::find_global(version);
    }
    return user == kUserHandle ? interfaces::find_user(version) : nullptr;
}

} // namespace sunrise::steam

namespace sunrise::steam::context {

/** Invalidates every caller-owned Steam context table. */
void advance_generation() noexcept {
    g_contextGeneration.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace sunrise::steam::context
