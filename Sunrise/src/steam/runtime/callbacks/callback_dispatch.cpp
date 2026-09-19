#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "../../../client/content/activity/activity_sdk_generation_worker.h"
#include "../../../client/content/activity/scriptable_catalog_worker.h"
#include "../../../client/content/investment/worker.h"
#include "../../../client/hooks/feature_flags/feature_flags.h"
#include "../../../client/hooks/instance_mutex/instance_mutex_release.h"
#include "../../../client/hooks/machine_id/machine_id_override.h"
#include "../../../client/hooks/net_tick_probe/net_tick_probe.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../server/runtime/server_runtime.h"
#include "../../interfaces/internal.h"
#include "../internal.h"
#include "callback_registry.h"

namespace sunrise::steam::runtime::callbacks {
namespace {

/** The server's own tick, matching the frame cadence the pump used to give it. */
constexpr std::uint64_t kServerTickMs = 10;
std::atomic_bool g_serverThreadStarted{false};

/**
 * Runs the server on its own thread. The game frame can block on a loopback send until the
 * server answers, so the game must never be the server's clock.
 */
DWORD WINAPI server_thread(LPVOID) noexcept {
    for (;;) {
        Sleep(static_cast<DWORD>(kServerTickMs));
        // The service's two-second timing report is also the thread's proof of life.
        server::service(GetTickCount64());
    }
}

/** Starts the server thread once, after the first activated slice. */
void start_server_thread_once() noexcept {
    if (g_serverThreadStarted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, &server_thread, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=core stage=pump result=server_thread");
    } else {
        core::log::write(core::log::Channel::server,
                         core::log::Level::error,
                         "ev=core stage=pump result=server_thread_fail");
    }
}

/** Steam callback vtable slots for its two Run overloads. */
enum class CallbackMethod : std::size_t {
    callResult = 0,
    regular = 1,
};

/** @return One method of a Steam-owned callback object, or null when the slot is empty. */
[[nodiscard]] void* callback_method(void* callback, CallbackMethod method) noexcept {
    auto** methods = *static_cast<void***>(callback);
    return methods != nullptr ? methods[static_cast<std::size_t>(method)] : nullptr;
}

/** Calls the regular callback overload through the Steam callback ABI. */
void invoke_callback(void* callback, void* payload) noexcept {
    void* const method = callback_method(callback, CallbackMethod::regular);
    if (method != nullptr) {
        const auto run = reinterpret_cast<void (*)(void*, void*)>(method);
        run(callback, payload);
    }
}

/** Calls the call-result overload through the Steam callback ABI. */
void invoke_call_result(void* callback, void* payload, ApiCall call) noexcept {
    void* const method = callback_method(callback, CallbackMethod::callResult);
    if (method != nullptr) {
        const auto run = reinterpret_cast<void (*)(void*, void*, bool, ApiCall)>(method);
        run(callback, payload, false, call);
    }
}

/**
 * Takes the oldest queued callback event. Runs under the callback lock.
 * @param event Receives one copied event.
 * @return True when an event was there.
 */
[[nodiscard]] bool pop_event(CallbackEvent& event) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_eventCount == 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    event = g_events[g_eventHead];
    g_events[g_eventHead] = {};
    g_eventHead = (g_eventHead + 1) % kEventCapacity;
    --g_eventCount;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Finds the registrations, then calls them only after the callback lock is released. */
void dispatch_event(CallbackEvent& event) noexcept {
    if (!event.guard.valid()) {
        return;
    }
    std::array<void*, kCallbackCapacity> callbacks{};
    std::array<void*, kCallResultCapacity> callResults{};
    std::size_t callbackCount{};
    std::size_t callResultCount{};
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& entry : g_callbacks) {
        if (entry.callback != nullptr && entry.callbackId == event.callbackId) {
            callbacks[callbackCount++] = entry.callback;
        }
    }
    if (event.call != 0) {
        for (auto& entry : g_callResults) {
            if (entry.callback != nullptr && entry.call == event.call
                && callback_id(entry.callback) == event.callbackId) {
                callResults[callResultCount++] = entry.callback;
                entry = {};
            }
        }
    }
    // Callback code can register again from inside the call, so calls happen after the unlock.
    ReleaseSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < callbackCount; ++index) {
        if (!event.guard.valid()) {
            return;
        }
        invoke_callback(callbacks[index], event.payload.data());
    }
    for (std::size_t index = 0; index < callResultCount; ++index) {
        if (!event.guard.valid()) {
            return;
        }
        invoke_call_result(callResults[index], event.payload.data(), event.call);
    }
}

/** Code of a fault that unwound out of the slice, reported by the next call. Zero means none. */
std::atomic<std::uint32_t> g_faultCode{0};
/** Times the game has called the pump. A frozen count means it stopped calling. */
std::atomic<std::uint64_t> g_callCount{0};

/**
 * Writes the fault line one call after the fault. Logging from the handler could take the log
 * lock the faulting slice still holds, so the report waits until that stack is gone.
 */
void report_fault_once() noexcept {
    const std::uint32_t code = g_faultCode.exchange(0, std::memory_order_relaxed);
    if (code == 0) {
        return;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=core stage=pump result=fault code=0x%08X", code);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::error,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace
} // namespace sunrise::steam::runtime::callbacks

namespace sunrise::steam {
namespace {

/** Runs one whole slice. Separated so the caller can wrap it in a structured handler. */
void run_slice() noexcept {
    // Presentation goes in first, so the sweep below has an overlay to draw with.
    runtime::activate_graphics_once();
    // The sweep stalls this drawing thread, so the overlay is raised on the call before it.
    if (runtime::main_activation_pending()
        && core::ui::busy::raise_early(core::ui::busy::Task::initialization)) {
        return;
    }
    // The network group must own SignOn before callback work can send it.
    const auto socialGeneration = interfaces::methods::friends_generation();
    const bool mainActive = runtime::activate_main_once();
    if (mainActive) {
        interfaces::methods::service_friends(socialGeneration);
        interfaces::methods::service_invites(socialGeneration);
        interfaces::methods::service_lobbies();
    }
    runtime::callbacks::CallbackEvent event;
    for (std::size_t count = 0;
         count < runtime::callbacks::kEventCapacity && runtime::callbacks::pop_event(event);
         ++count) {
        runtime::callbacks::dispatch_event(event);
    }
    if (mainActive) {
        const auto now = GetTickCount64();
        if (core::settings::multiplayer()) {
            client::hooks::machine_id::poll();
            client::hooks::instance_mutex::release_once();
        }
        // Reached only once the game is activated, which is when its feature registry exists.
        client::hooks::feature_flags::apply_once();
        // Samples the healthy cadence. The assert observer samples it again once this tick stops.
        client::hooks::net_tick_probe::sample();
        // Everything below must run on the game's thread; the server gets its own from here on.
        runtime::callbacks::start_server_thread_once();
        client::content::investment::worker::service(now);
        client::content::activity::sdk_generation::service();
        client::content::activity::scriptables::service();
    }
}

} // namespace

/**
 * Delivers one capped batch of queued callbacks on the caller thread. The game holds an unguarded
 * re-entrancy latch across this call, so a fault unwinding out of here kills every later tick.
 */
void run_callbacks() noexcept {
    runtime::callbacks::g_callCount.fetch_add(1, std::memory_order_relaxed);
    runtime::callbacks::report_fault_once();
    __try {
        run_slice();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime::callbacks::g_faultCode.store(static_cast<std::uint32_t>(GetExceptionCode()),
                                              std::memory_order_relaxed);
    }
}

/** Times the game has called `run_callbacks`. */
std::uint64_t callback_count() noexcept {
    return runtime::callbacks::g_callCount.load(std::memory_order_relaxed);
}

/** Copies one callback payload into the delivery queue. */
bool queue_callback(int callbackId,
                    ApiCall call,
                    const void* payload,
                    std::size_t payloadSize) noexcept {
    const CallbackDelivery delivery{callbackId, call, payload, payloadSize};
    return queue_callbacks(std::span(&delivery, 1));
}

bool queue_callbacks(std::span<const CallbackDelivery> deliveries) noexcept {
    using namespace runtime::callbacks;
    if (deliveries.size() > kEventCapacity) {
        return false;
    }
    for (const auto& delivery : deliveries) {
        if (delivery.callbackId <= 0 || delivery.payloadSize > kEventPayloadCapacity
            || (delivery.payload == nullptr && delivery.payloadSize != 0)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::error,
                             "ev=callback_queue result=invalid");
            return false;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& delivery : deliveries) {
        if (!delivery.guard.valid()) {
            ReleaseSRWLockExclusive(&g_lock);
            return false;
        }
    }
    if (deliveries.size() > kEventCapacity - g_eventCount) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(
            core::log::Channel::client, core::log::Level::error, "ev=callback_queue result=full");
        return false;
    }
    for (const auto& delivery : deliveries) {
        const std::size_t tail = (g_eventHead + g_eventCount) % kEventCapacity;
        auto& event = g_events[tail];
        event = {};
        event.callbackId = delivery.callbackId;
        event.call = delivery.call;
        event.payloadSize = delivery.payloadSize;
        event.guard = delivery.guard;
        if (delivery.payloadSize != 0) {
            std::memcpy(event.payload.data(), delivery.payload, delivery.payloadSize);
        }
        ++g_eventCount;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

} // namespace sunrise::steam
