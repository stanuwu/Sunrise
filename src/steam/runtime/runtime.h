#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace sunrise::steam {

/** Steam user handle the shim hands out. */
using UserHandle = std::int32_t;

/** Steam pipe handle the shim hands out. */
using PipeHandle = std::int32_t;

/** Always-rising id for one async API result. */
using ApiCall = std::uint64_t;

/**
 * Starts the Steam shim and its exported state.
 * @param module Module handle used to set up Core paths and logging.
 * @return True when the Steam shim and the Core runtime are ready.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/**
 * Stops callback delivery and clears Steam state.
 * @return True when every active subsystem shuts down cleanly.
 */
[[nodiscard]] bool shutdown() noexcept;

/** @return True. The in-process Steam provider stays up for the whole DLL lifetime. */
[[nodiscard]] bool is_running() noexcept;

/** Delivers one batch of queued callbacks on the caller thread. The batch has a size cap. */
void run_callbacks() noexcept;

/** @param appId Application id the Steam shim reports. */
void set_app_id(DWORD appId) noexcept;

/** @return Current application id. */
[[nodiscard]] DWORD app_id() noexcept;

/** @return Next nonzero async API call id. */
[[nodiscard]] ApiCall next_api_call() noexcept;

/**
 * Copies one callback payload into the delivery queue.
 * @param call Matching API call, or zero for a regular callback.
 * @return True when the event fits and the queue has room.
 */
[[nodiscard]] bool
queue_callback(int callbackId, ApiCall call, const void* payload, std::size_t payloadSize) noexcept;

/** @return The shim's single valid user handle. */
[[nodiscard]] UserHandle user_handle() noexcept;

/** @return The shim's single valid pipe handle. */
[[nodiscard]] PipeHandle pipe_handle() noexcept;

/** Registers one Steam-owned callback object for a callback id. */
void register_callback(void* callback, int callbackId) noexcept;

/** @param callback Steam-owned object to remove from every registration. */
void unregister_callback(void* callback) noexcept;

/** Registers one Steam-owned call-result object for a nonzero API call. */
void register_call_result(void* callback, ApiCall call) noexcept;

/**
 * Removes an API call-result registration.
 * @param call API call to remove, or zero to remove every call for the object.
 */
void unregister_call_result(void* callback, ApiCall call) noexcept;

/**
 * Sets up a Steam internal context table in caller storage.
 * @param data Steam-owned context table prefix.
 * @return Address of the filled interface field, or null for bad input.
 */
[[nodiscard]] void* context_init(void* data) noexcept;

/**
 * Creates one supported Steam interface.
 * @param callerAddress Return address caught at the exported factory boundary.
 * @return Interface object, or null when the version is not supported.
 */
[[nodiscard]] void* create_interface(const char* version, const void* callerAddress) noexcept;

/**
 * Returns one supported user interface for a valid user handle.
 * @param user The zero process-global handle, or the shim's local user handle.
 * @return Interface object, or null when the handle or version is not supported.
 */
[[nodiscard]] void* find_or_create_user_interface(UserHandle user, const char* version) noexcept;

} // namespace sunrise::steam
