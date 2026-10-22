#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <detours.h>
#include <span>

#include "../../../process/freeze/client_process_freeze.h"
#include "../../detour.h"
#include "detour_transaction_limits.h"

namespace sunrise::client::hooking::detour::transaction {

/** Fixed limit on the handles held while process threads are suspended. */
constexpr std::size_t kThreadLimit = limits::kThreadCount;
/** Most hooks one transaction can hold. */
inline constexpr std::size_t kBatchLimit = limits::kOperationCount;

/**
 * Queues every detour removal against temporary trampoline pointers.
 * @param handles Attached handles in stable transaction order.
 * @param originals Receives pointer values Detours uses through commit.
 * @return True when Detours accepted every removal.
 */
[[nodiscard]] inline bool queue_detach(std::span<const Handle> handles,
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

/** @param handles Handles to clear, only after their removal commits. */
inline void clear_handles(std::span<Handle> handles) noexcept {
    for (Handle& handle : handles) {
        handle = {};
    }
}

/** @return True when the batch fits one transaction and every handle is attached. */
[[nodiscard]] inline bool valid_detach_batch(std::span<const Handle> handles) noexcept {
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

/** Owns the process thread handles enlisted in one Detours transaction. */
struct Threads {
    std::array<HANDLE, kThreadLimit> handles{};
    std::array<DWORD, kThreadLimit> ids{};
    std::size_t count{};
    process::freeze::ProcessLocks processLocks{};
};

/** Result of checking suspended instruction pointers. */
enum class InspectionResult {
    clear,
    protectedCodeActive,
    failed,
};

/** Starts a transaction and suspends every observed process thread. */
[[nodiscard]] bool begin(Threads& threads) noexcept;

/** Aborts the current transaction and releases its thread handles. */
[[nodiscard]] bool abort(Threads& threads) noexcept;

/** Commits the current transaction and releases its thread handles. */
[[nodiscard]] bool commit(Threads& threads) noexcept;

/** Checks every suspended instruction pointer against the protected functions and spans. */
[[nodiscard]] InspectionResult inspect(const Threads& threads,
                                       std::span<const ProtectedCodeEntry> entries,
                                       std::span<const ProtectedCodeSpan> spans = {}) noexcept;

} // namespace sunrise::client::hooking::detour::transaction
