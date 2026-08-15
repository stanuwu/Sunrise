#include "callback_registry.h"

#include <cstdint>
#include <cstring>

#include "../../../core/logging/log.h"

namespace sunrise::steam::runtime::callbacks {
namespace {

/** In Steam's callback base, the flags follow the vtable pointer. */
constexpr std::size_t kCallbackFlagsOffset = sizeof(void*);
/** The callback id follows the pointer and the 32-bit flags field. */
constexpr std::size_t kCallbackIdOffset = sizeof(void*) + sizeof(std::uint32_t);
/** Steam's flag bit that marks an object as registered. */
constexpr std::uint8_t kRegisteredFlag = 0x01;

/** Stores a callback id in Steam-owned callback storage. */
void set_callback_id(void* callback, int callbackId) noexcept {
    auto* bytes = static_cast<std::byte*>(callback);
    std::memcpy(bytes + kCallbackIdOffset, &callbackId, sizeof(callbackId));
}

} // namespace

SRWLOCK g_lock{SRWLOCK_INIT};
std::array<CallbackEntry, kCallbackCapacity> g_callbacks{};
std::array<CallResultEntry, kCallResultCapacity> g_callResults{};
std::array<CallbackEvent, kEventCapacity> g_events{};
std::size_t g_eventHead{};
std::size_t g_eventCount{};

/** Updates the registered bit in Steam-owned callback storage. */
void set_registration(void* callback, bool registered) noexcept {
    if (callback == nullptr) {
        return;
    }
    auto* bytes = static_cast<std::byte*>(callback);
    std::uint8_t flags{};
    std::memcpy(&flags, bytes + kCallbackFlagsOffset, sizeof(flags));
    flags = registered ? static_cast<std::uint8_t>(flags | kRegisteredFlag)
                       : static_cast<std::uint8_t>(flags & ~kRegisteredFlag);
    std::memcpy(bytes + kCallbackFlagsOffset, &flags, sizeof(flags));
}

/** Reads a callback id from Steam-owned callback storage. */
int callback_id(void* callback) noexcept {
    int callbackId{};
    const auto* bytes = static_cast<const std::byte*>(callback);
    std::memcpy(&callbackId, bytes + kCallbackIdOffset, sizeof(callbackId));
    return callbackId;
}

} // namespace sunrise::steam::runtime::callbacks

namespace sunrise::steam {

/** Registers one Steam-owned callback object for a callback id. */
void register_callback(void* callback, int callbackId) noexcept {
    using namespace runtime::callbacks;
    if (callback == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    CallbackEntry* freeEntry = nullptr;
    for (auto& entry : g_callbacks) {
        if (entry.callback == callback) {
            entry.callbackId = callbackId;
            set_callback_id(callback, callbackId);
            set_registration(callback, true);
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (freeEntry == nullptr && entry.callback == nullptr) {
            freeEntry = &entry;
        }
    }
    if (freeEntry != nullptr) {
        *freeEntry = CallbackEntry{callback, callbackId};
        set_callback_id(callback, callbackId);
        set_registration(callback, true);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (freeEntry == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=callback_register result=full");
    }
}

/** @param callback Steam-owned object to remove from every registration. */
void unregister_callback(void* callback) noexcept {
    using namespace runtime::callbacks;
    if (callback == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (auto& entry : g_callbacks) {
        if (entry.callback == callback) {
            entry = {};
        }
    }
    set_registration(callback, false);
    ReleaseSRWLockExclusive(&g_lock);
}

/** Registers one Steam-owned call-result object for a nonzero API call. */
void register_call_result(void* callback, ApiCall call) noexcept {
    using namespace runtime::callbacks;
    if (callback == nullptr || call == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    CallResultEntry* freeEntry = nullptr;
    for (auto& entry : g_callResults) {
        if (entry.callback == callback && entry.call == call) {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (freeEntry == nullptr && entry.callback == nullptr) {
            freeEntry = &entry;
        }
    }
    if (freeEntry != nullptr) {
        *freeEntry = CallResultEntry{callback, call};
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (freeEntry == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=call_result_register result=full");
    }
}

/** Removes an API call-result registration. */
void unregister_call_result(void* callback, ApiCall call) noexcept {
    using namespace runtime::callbacks;
    if (callback == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (auto& entry : g_callResults) {
        if (entry.callback == callback && (call == 0 || entry.call == call)) {
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

namespace runtime::callbacks {

/** Clears every callback, call-result and queued-event registration. */
void clear() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (auto& entry : g_callbacks) {
        if (entry.callback != nullptr) {
            set_registration(entry.callback, false);
        }
        entry = {};
    }
    g_callResults.fill({});
    g_events.fill({});
    g_eventHead = 0;
    g_eventCount = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace runtime::callbacks
} // namespace sunrise::steam
