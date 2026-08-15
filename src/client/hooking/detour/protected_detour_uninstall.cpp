#include <Windows.h>

#include <array>
#include <detours.h>

#include "../detour.h"
#include "transaction/detour_thread_transaction.h"

namespace sunrise::client::hooking::detour {
namespace {

/** 32 hook slots bound each fixed-storage Detours transaction. */
constexpr std::size_t kBatchLimit = 32;

/** @param handles Detours handles. @return True when the protected batch is usable. */
[[nodiscard]] bool valid_batch(std::span<const Handle> handles) noexcept {
    if (handles.empty() || handles.size() > kBatchLimit) {
        return false;
    }
    for (const Handle& handle : handles) {
        if (!handle.attached || handle.original == nullptr || handle.replacement == nullptr) {
            return false;
        }
    }
    return true;
}

/**
 * Queues protected detour removal against transaction-local trampoline pointers.
 * @param handles Attached handles in stable order.
 * @param originals Receives trampoline pointers held through commit.
 * @return True when every removal was accepted by Detours.
 */
[[nodiscard]] bool queue_detach(std::span<const Handle> handles,
                                std::array<void*, kBatchLimit>& originals) noexcept {
    originals = {};
    for (std::size_t index = 0; index < handles.size(); ++index) {
        originals[index] = handles[index].original;
        if (DetourDetach(&originals[index], handles[index].replacement) != NO_ERROR) {
            return false;
        }
    }
    return true;
}

/** @param handles Successfully removed handles to clear. */
void clear_handles(std::span<Handle> handles) noexcept {
    for (Handle& handle : handles) {
        handle = {};
    }
}

} // namespace

/**
 * Detaches one hook only once every given protected function is idle.
 * @param handle Attached detour handle.
 * @param protectedEntries Function entries that must be idle before removal.
 * @return Removed, active, or failed without changing the handle on failure.
 */
UninstallResult uninstall(Handle& handle,
                          std::span<const ProtectedCodeEntry> protectedEntries) noexcept {
    return uninstall(std::span(&handle, 1), protectedEntries);
}

/**
 * Detaches one hook only once protected code and caller-owned work are idle.
 * @param handle Attached detour handle.
 * @param protectedEntries Function entries that must be idle before removal.
 * @param idleCheck Lock-free idle test, run with the transaction threads suspended.
 * @return Removed, active, or failed without changing the handle on failure.
 */
UninstallResult uninstall(Handle& handle,
                          std::span<const ProtectedCodeEntry> protectedEntries,
                          IdleCheck idleCheck) noexcept {
    return uninstall(std::span(&handle, 1), protectedEntries, idleCheck);
}

/**
 * Detaches a batch only when every given protected function is idle.
 * @param handles Attached handles in their original transaction order.
 * @param protectedEntries Function entries that must be idle before removal.
 * @return Removed, active, or failed. Handles are kept unless they were removed.
 */
UninstallResult uninstall(std::span<Handle> handles,
                          std::span<const ProtectedCodeEntry> protectedEntries) noexcept {
    return uninstall(handles, protectedEntries, nullptr);
}

/**
 * Detaches a batch only when protected code and caller-owned work are idle.
 * @param handles Attached handles in their original transaction order.
 * @param protectedEntries Function entries that must be idle before removal.
 * @param idleCheck Lock-free idle test, run with the transaction threads suspended.
 * @return Removed, active, or failed. Handles are kept unless they were removed.
 */
UninstallResult uninstall(std::span<Handle> handles,
                          std::span<const ProtectedCodeEntry> protectedEntries,
                          IdleCheck idleCheck) noexcept {
    if (!valid_batch(handles) || protectedEntries.empty()) {
        return UninstallResult::failed;
    }

    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return UninstallResult::failed;
    }

    const transaction::InspectionResult inspection =
        transaction::inspect(threads, protectedEntries);
    if (inspection != transaction::InspectionResult::clear) {
        const bool aborted = transaction::abort(threads);
        if (!aborted || inspection == transaction::InspectionResult::failed) {
            return UninstallResult::failed;
        }
        return UninstallResult::protectedCodeActive;
    }
    if (idleCheck != nullptr && !idleCheck()) {
        return transaction::abort(threads) ? UninstallResult::protectedCodeActive
                                           : UninstallResult::failed;
    }

    std::array<void*, kBatchLimit> originals{};
    if (!queue_detach(handles, originals)) {
        (void)transaction::abort(threads);
        return UninstallResult::failed;
    }
    if (!transaction::commit(threads)) {
        return UninstallResult::failed;
    }

    clear_handles(handles);
    return UninstallResult::removed;
}

} // namespace sunrise::client::hooking::detour
