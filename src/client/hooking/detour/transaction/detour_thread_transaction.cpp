#include "detour_thread_transaction.h"

#include <Windows.h>

#include <TlHelp32.h>
#include <detours.h>

#include "../../../process/freeze/client_process_freeze.h"

namespace sunrise::client::hooking::detour::transaction {
namespace {

/** 4 protected functions per hook bound the fixed range storage, so no heap is used. */
constexpr std::size_t kProtectedCodeLimit = 64;

/** Exact executable range described by one x64 unwind record. */
struct CodeRange {
    DWORD64 begin{};
    DWORD64 end{};
};

/**
 * Closes every held thread handle after Detours resumes the threads.
 * @param threads Thread handles to close and clear.
 */
void close_threads(Threads& threads) noexcept {
    for (std::size_t index = 0; index < threads.count; ++index) {
        CloseHandle(threads.handles[index]);
    }
    threads = {};
}

/**
 * Checks whether one thread id was already enlisted by an earlier snapshot.
 * @param threads Threads kept suspended by the active transaction.
 * @param threadId Candidate process thread id.
 * @return True when the thread is already enlisted.
 */
[[nodiscard]] bool contains(const Threads& threads, DWORD threadId) noexcept {
    for (std::size_t index = 0; index < threads.count; ++index) {
        if (threads.ids[index] == threadId) {
            return true;
        }
    }
    return false;
}

/**
 * Enlists every unseen thread present in one process-wide snapshot.
 * @param threads Receives handles that stay suspended until the transaction ends.
 * @param foundUnseen Receives true when this pass saw any new thread id.
 * @return True when the whole snapshot was inspected without a hard failure.
 */
[[nodiscard]] bool enlist_snapshot(Threads& threads, bool& foundUnseen) noexcept {
    foundUnseen = false;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    BOOL available = Thread32First(snapshot, &entry);
    const DWORD processId = GetCurrentProcessId();
    const DWORD currentThreadId = GetCurrentThreadId();
    bool succeeded = true;
    while (available != FALSE && succeeded) {
        const bool belongsToProcess = entry.th32OwnerProcessID == processId;
        const bool needsEnlistment =
            entry.th32ThreadID != currentThreadId && !contains(threads, entry.th32ThreadID);
        if (belongsToProcess && needsEnlistment) {
            foundUnseen = true;
            if (threads.count == threads.handles.size()) {
                succeeded = false;
                break;
            }

            const HANDLE thread =
                OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                           FALSE,
                           entry.th32ThreadID);
            if (thread == nullptr) {
                // A disappearing thread is absent from the next stable snapshot.
                if (GetLastError() != ERROR_INVALID_PARAMETER) {
                    succeeded = false;
                }
            } else if (DetourUpdateThread(thread) != NO_ERROR) {
                CloseHandle(thread);
                succeeded = false;
            } else {
                threads.handles[threads.count] = thread;
                threads.ids[threads.count] = entry.th32ThreadID;
                ++threads.count;
            }
        }
        available = Thread32Next(snapshot, &entry);
    }

    if (succeeded && available == FALSE && GetLastError() != ERROR_NO_MORE_FILES) {
        succeeded = false;
    }
    CloseHandle(snapshot);
    return succeeded;
}

/**
 * Enlists new process threads until a full snapshot finds no unseen thread id.
 * @param threads Receives every handle the transaction holds.
 * @return True when a full pass found no new thread.
 */
[[nodiscard]] bool enlist_until_stable(Threads& threads) noexcept {
    bool foundUnseen{};
    do {
        if (!enlist_snapshot(threads, foundUnseen)) {
            return false;
        }
        // Earlier handles stay suspended while a later pass finds newly created threads.
    } while (foundUnseen);
    return true;
}

/** @param protection Windows page protection. @return True for executable page types. */
[[nodiscard]] bool is_executable(DWORD protection) noexcept {
    /** The low byte stores PAGE_* type while higher bits store modifiers. */
    constexpr DWORD kPageTypeMask = 0xFF;
    switch (protection & kPageTypeMask) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

/**
 * Finds the canonical unwind-backed function range of one protected entry.
 * @param entry Protected function entry given by the hook owner.
 * @param range Receives the exact executable range.
 * @return True when both the entry and canonical code have a valid x64 unwind record.
 */
[[nodiscard]] bool resolve_range(const ProtectedCodeEntry& entry, CodeRange& range) noexcept {
    range = {};
    if (entry.address == nullptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION entryMemory{};
    if (VirtualQuery(entry.address, &entryMemory, sizeof(entryMemory)) != sizeof(entryMemory)
        || entryMemory.State != MEM_COMMIT || !is_executable(entryMemory.Protect)) {
        return false;
    }

    void* const code = DetourCodeFromPointer(entry.address, nullptr);
    MEMORY_BASIC_INFORMATION codeMemory{};
    if (code == nullptr || VirtualQuery(code, &codeMemory, sizeof(codeMemory)) != sizeof(codeMemory)
        || codeMemory.State != MEM_COMMIT || !is_executable(codeMemory.Protect)) {
        return false;
    }

    const DWORD64 codeAddress = reinterpret_cast<DWORD64>(code);
    DWORD64 imageBase{};
    const RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(codeAddress, &imageBase, nullptr);
    if (function == nullptr) {
        return false;
    }

    range = {imageBase + function->BeginAddress, imageBase + function->EndAddress};
    return range.begin < range.end && codeAddress >= range.begin && codeAddress < range.end;
}

} // namespace

/** Starts a Detours transaction and enlists process threads to a stable snapshot. */
bool begin(Threads& threads) noexcept {
    threads = {};
    // Detours suspends these threads at commit. Another suspender running at the same time
    // would freeze this thread, and then neither side can finish.
    process::freeze::enter_exclusive();
    if (DetourTransactionBegin() != NO_ERROR) {
        process::freeze::leave_exclusive();
        return false;
    }
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR || !enlist_until_stable(threads)) {
        (void)DetourTransactionAbort();
        close_threads(threads);
        process::freeze::leave_exclusive();
        return false;
    }
    return true;
}

/** Aborts the active Detours transaction before releasing enlisted thread handles. */
bool abort(Threads& threads) noexcept {
    const bool aborted = DetourTransactionAbort() == NO_ERROR;
    close_threads(threads);
    process::freeze::leave_exclusive();
    return aborted;
}

/** Commits the active Detours transaction before releasing enlisted thread handles. */
bool commit(Threads& threads) noexcept {
    const bool committed = DetourTransactionCommit() == NO_ERROR;
    close_threads(threads);
    process::freeze::leave_exclusive();
    return committed;
}

/** Finds the protected function ranges and checks every suspended instruction pointer. */
InspectionResult inspect(const Threads& threads,
                         std::span<const ProtectedCodeEntry> entries) noexcept {
    if (entries.empty() || entries.size() > kProtectedCodeLimit) {
        return InspectionResult::failed;
    }

    std::array<CodeRange, kProtectedCodeLimit> ranges{};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!resolve_range(entries[index], ranges[index])) {
            return InspectionResult::failed;
        }
    }

    for (std::size_t threadIndex = 0; threadIndex < threads.count; ++threadIndex) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(threads.handles[threadIndex], &context) == FALSE) {
            return InspectionResult::failed;
        }
        for (std::size_t rangeIndex = 0; rangeIndex < entries.size(); ++rangeIndex) {
            const CodeRange range = ranges[rangeIndex];
            if (context.Rip >= range.begin && context.Rip < range.end) {
                return InspectionResult::protectedCodeActive;
            }
        }
    }
    return InspectionResult::clear;
}

} // namespace sunrise::client::hooking::detour::transaction
