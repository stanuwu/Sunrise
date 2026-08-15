#include <Windows.h>

#include <cstring>

#include "../../../client/content/investment/worker.h"
#include "../../../core/logging/log.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../server/runtime/server_runtime.h"
#include "../internal.h"
#include "callback_registry.h"

namespace sunrise::steam::runtime::callbacks {
namespace {

/** Steam callback vtable slots for its two Run overloads. */
enum class CallbackMethod : std::size_t {
    callResult = 0,
    regular = 1,
};

/** Calls the regular callback overload through the Steam callback ABI. */
void invoke_callback(void* callback, void* payload) noexcept {
    auto** methods = *static_cast<void***>(callback);
    const auto slot = static_cast<std::size_t>(CallbackMethod::regular);
    if (methods == nullptr || methods[slot] == nullptr) {
        return;
    }
    const auto run = reinterpret_cast<void (*)(void*, void*)>(methods[slot]);
    run(callback, payload);
}

/** Calls the call-result overload through the Steam callback ABI. */
void invoke_call_result(void* callback, void* payload, ApiCall call) noexcept {
    auto** methods = *static_cast<void***>(callback);
    const auto slot = static_cast<std::size_t>(CallbackMethod::callResult);
    if (methods == nullptr || methods[slot] == nullptr) {
        return;
    }
    const auto run = reinterpret_cast<void (*)(void*, void*, bool, ApiCall)>(methods[slot]);
    run(callback, payload, false, call);
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
        invoke_callback(callbacks[index], event.payload.data());
    }
    for (std::size_t index = 0; index < callResultCount; ++index) {
        invoke_call_result(callResults[index], event.payload.data(), event.call);
    }
}

} // namespace
} // namespace sunrise::steam::runtime::callbacks

namespace sunrise::steam {

/** Delivers one batch of queued callbacks on the caller thread. The batch has a size cap. */
void run_callbacks() noexcept {
    // Presentation goes in first, so the sweep below has an overlay to draw with. It only hooks
    // Present; the game still decides when a frame is drawn.
    runtime::activate_graphics_once();
    // The sweep stalls this thread, and this thread is the one that draws. The overlay is raised
    // on this call, so a frame carrying it reaches the screen before the stall.
    if (runtime::main_activation_pending()
        && core::ui::busy::raise_early(core::ui::busy::Task::initialization)) {
        return;
    }
    // The network group must own SignOn before callback work can send it.
    const bool mainActive = runtime::activate_main_once();
    runtime::callbacks::CallbackEvent event;
    for (std::size_t count = 0;
         count < runtime::callbacks::kEventCapacity && runtime::callbacks::pop_event(event);
         ++count) {
        runtime::callbacks::dispatch_event(event);
    }
    if (mainActive) {
        const auto now = GetTickCount64();
        server::service(now);
        client::content::investment::worker::service(now);
    }
}

/** Copies one callback payload into the delivery queue. */
bool queue_callback(int callbackId,
                    ApiCall call,
                    const void* payload,
                    std::size_t payloadSize) noexcept {
    using namespace runtime::callbacks;
    if (callbackId <= 0 || payloadSize > kEventPayloadCapacity
        || (payload == nullptr && payloadSize != 0)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=callback_queue result=invalid");
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_eventCount == kEventCapacity) {
        ReleaseSRWLockExclusive(&g_lock);
        core::log::write(
            core::log::Channel::client, core::log::Level::error, "ev=callback_queue result=full");
        return false;
    }
    // Head plus count names the only free ring slot while the lock is held.
    const std::size_t tail = (g_eventHead + g_eventCount) % kEventCapacity;
    auto& event = g_events[tail];
    event = {};
    event.callbackId = callbackId;
    event.call = call;
    event.payloadSize = payloadSize;
    if (payloadSize != 0) {
        std::memcpy(event.payload.data(), payload, payloadSize);
    }
    ++g_eventCount;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

} // namespace sunrise::steam
