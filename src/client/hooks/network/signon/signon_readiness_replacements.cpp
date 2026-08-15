#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../coordinator/network_call_coordinator.h"
#include "../platform.h"

namespace sunrise::client::hooks::network::signon {
namespace {

using ReadinessFailure = bool(__fastcall*)() noexcept;
using ReadinessReady = bool(__fastcall*)() noexcept;

/** A fixed log-line size keeps stack use small. */
constexpr std::size_t kReadinessEventCapacity = 96;

/**
 * Logs one readiness check, so a silent boot names the stage it stopped at.
 * @param stage Stage token for the log line.
 * @param nativeResult Result returned by the native check.
 * @param forced Whether Sunrise replaced that result.
 */
void log_readiness(std::string_view stage, bool nativeResult, bool forced) noexcept {
    std::array<char, kReadinessEventCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=signon stage=%.*s native=%u forced=%u result=ok",
                                      static_cast<int>(stage.size()),
                                      stage.data(),
                                      nativeResult ? 1U : 0U,
                                      forced ? 1U : 0U);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::debug, {line.data(), length});
}

/**
 * Still runs the native readiness work while Sunrise owns the local SignOn path.
 * @return False while Sunrise owns local SignOn, otherwise the native result.
 */
__declspec(noinline) bool __fastcall readiness_body() noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::signOnReadinessFailure, coordinator::ConsumerKind::none);
    bool result = true;
    __try {
        const auto call = reinterpret_cast<ReadinessFailure>(lease.original);
        if (call != nullptr) {
            result = call();
        }
    } __finally {
        coordinator::g_callEgress();
    }
    log_readiness("readiness_failure", result, lease.accepting);
    return lease.accepting ? false : result;
}

/**
 * Still runs the native readiness work while Sunrise supplies the local transport.
 * @return True while Sunrise supplies the transport, otherwise the native result.
 */
__declspec(noinline) bool __fastcall ready_body() noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::signOnReadinessReady, coordinator::ConsumerKind::none);
    bool result{};
    __try {
        const auto call = reinterpret_cast<ReadinessReady>(lease.original);
        if (call != nullptr) {
            result = call();
        }
    } __finally {
        coordinator::g_callEgress();
    }
    log_readiness("readiness_ready", result, lease.accepting);
    return lease.accepting ? true : result;
}

} // namespace

/** @return The internal-linkage readiness body, kept safe while the detour is removed. */
void* readiness_entry_point() noexcept {
    return reinterpret_cast<void*>(&readiness_body);
}

/** @return The internal-linkage ready body, kept safe while the detour is removed. */
void* ready_entry_point() noexcept {
    return reinterpret_cast<void*>(&ready_body);
}

} // namespace sunrise::client::hooks::network::signon
