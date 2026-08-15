#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

#include "../runtime.h"

namespace sunrise::steam::runtime::callbacks {

/** Clears every callback, call-result and queued-event registration. */
void clear() noexcept;

/** A fixed cap keeps callback state off the heap. */
inline constexpr std::size_t kCallbackCapacity = 256;
/** Call-result cap. It matches the callback cap. */
inline constexpr std::size_t kCallResultCapacity = 256;
/** One callback pump handles at most this many queued events. */
inline constexpr std::size_t kEventCapacity = 64;
/** The largest payload among the Steam callback structs used here. */
inline constexpr std::size_t kEventPayloadCapacity = 32;

/** One regular callback registration. */
struct CallbackEntry {
    void* callback{};
    int callbackId{};
};

/** One async call-result registration. */
struct CallResultEntry {
    void* callback{};
    ApiCall call{};
};

/** One copied callback event waiting for delivery on the caller thread. */
struct CallbackEvent {
    int callbackId{};
    ApiCall call{};
    std::size_t payloadSize{};
    std::array<std::byte, kEventPayloadCapacity> payload{};
};

extern SRWLOCK g_lock;
extern std::array<CallbackEntry, kCallbackCapacity> g_callbacks;
extern std::array<CallResultEntry, kCallResultCapacity> g_callResults;
extern std::array<CallbackEvent, kEventCapacity> g_events;
extern std::size_t g_eventHead;
extern std::size_t g_eventCount;

/** Updates the registered bit in Steam-owned callback storage. */
void set_registration(void* callback, bool registered) noexcept;

/** @return The callback id stored in Steam-owned callback storage. */
[[nodiscard]] int callback_id(void* callback) noexcept;
} // namespace sunrise::steam::runtime::callbacks
