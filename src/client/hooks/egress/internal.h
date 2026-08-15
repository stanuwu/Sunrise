#pragma once

#include <array>
#include <cstddef>

#include "../../hooking/detour.h"
#include "platform/sdk.h"

namespace sunrise::client::hooks::egress {

/** Stable slot order shared by exports, Detours handles, and tests. */
enum class HookSlot : std::size_t {
    connect,
    wsaConnect,
    wsaConnectByNameA,
    wsaConnectByNameW,
    wsaConnectByList,
    send,
    wsaSend,
    sendTo,
    wsaSendTo,
    wsaSendMsg,
    wsaSendDisconnect,
    wsaJoinLeaf,
    transmitFile,
    wsaIoctl,
    getAddrInfoA,
    getAddrInfoW,
    getAddrInfoExA,
    getAddrInfoExW,
    getHostByName,
    getHostByAddress,
    getNameInfoA,
    getNameInfoW,
    dnsQueryA,
    dnsQueryW,
    dnsQueryUtf8,
    dnsQueryEx,
    recv,
    wsaRecv,
    recvFrom,
    wsaRecvFrom,
    dnsQueryRaw,
    count,
};

/** Fixed handle count covers every required and OS-optional egress entry point. */
inline constexpr std::size_t kHookCount = static_cast<std::size_t>(HookSlot::count);

extern SRWLOCK g_lock;
extern std::array<hooking::detour::Handle, kHookCount> g_handles;

/**
 * Returns one typed trampoline after the atomic install publishes its handles.
 * @tparam Function Exact Windows SDK function pointer type.
 * @return Callable original entry, or null before publication.
 */
template <typename Function> [[nodiscard]] Function original(HookSlot slot) noexcept {
    const auto index = static_cast<std::size_t>(slot);
    return reinterpret_cast<Function>(g_handles[index].original);
}

} // namespace sunrise::client::hooks::egress
