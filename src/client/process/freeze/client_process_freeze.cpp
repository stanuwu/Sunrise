#include "client_process_freeze.h"

#include <Windows.h>

#include <TlHelp32.h>

#include "../../../core/logging/log.h"
#include "../../diagnostics/module_range.h"

namespace sunrise::client::process::freeze {
namespace {

/** Attempts before the hold gives up and the work runs with the game still moving. */
constexpr std::size_t kAttemptLimit = 8;
/** One millisecond lets a rejected thread leave the code that rejected it. */
constexpr DWORD kRetryMilliseconds = 1;
/** SuspendThread reports this count on failure. */
constexpr DWORD kSuspendFailed = static_cast<DWORD>(-1);
/** NTSTATUS success. */
constexpr LONG kStatusSuccess = 0;
/** Zero flags block until the loader lock is owned. */
constexpr ULONG kBlockingLoaderLock = 0;

using LockLoaderFunction = LONG(NTAPI*)(ULONG flags, ULONG* state, ULONG_PTR* cookie);
using UnlockLoaderFunction = LONG(NTAPI*)(ULONG flags, ULONG_PTR cookie);

/** ntdll's loader lock entry points. */
struct LoaderLock {
    LockLoaderFunction lock{};
    UnlockLoaderFunction unlock{};
};

SRWLOCK g_exclusiveLock{SRWLOCK_INIT};

/** @return Resolved loader lock entry points, or cleared ones. */
[[nodiscard]] LoaderLock resolve_loader() noexcept {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return {};
    }
    return {reinterpret_cast<LockLoaderFunction>(GetProcAddress(ntdll, "LdrLockLoaderLock")),
            reinterpret_cast<UnlockLoaderFunction>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"))};
}

/** @return Loader lock entry points, resolved once while every thread still runs. */
[[nodiscard]] const LoaderLock& loader() noexcept {
    static const LoaderLock resolved = resolve_loader();
    return resolved;
}

/** @return Address range of the Sunrise image, or a cleared range. */
[[nodiscard]] diagnostics::ModuleRange own_range() noexcept {
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&own_range), &module) == FALSE) {
        return {};
    }
    diagnostics::ModuleRange range{};
    (void)diagnostics::module_range(module, range);
    return range;
}

/**
 * Takes the two process locks a held thread must never own.
 * The loader takes its own lock before the heap, so they are taken in that order here. Both
 * are recursive, so this thread still loads modules and allocates while they are held.
 * @param held Receives the loader cookie and both ownership flags.
 * @return True when this thread owns both locks.
 */
[[nodiscard]] bool take_locks(Held& held) noexcept {
    ULONG state = 0;
    ULONG_PTR cookie = 0;
    if (loader().lock(kBlockingLoaderLock, &state, &cookie) != kStatusSuccess) {
        return false;
    }
    held.loaderCookie = cookie;
    held.loaderLocked = true;
    if (HeapLock(GetProcessHeap()) == FALSE) {
        held.loaderLocked = false;
        (void)loader().unlock(kBlockingLoaderLock, cookie);
        return false;
    }
    held.heapLocked = true;
    return true;
}

/** @param held Lock ownership to drop, innermost first. */
void drop_locks(Held& held) noexcept {
    if (held.heapLocked) {
        held.heapLocked = false;
        (void)HeapUnlock(GetProcessHeap());
    }
    if (held.loaderLocked) {
        held.loaderLocked = false;
        (void)loader().unlock(kBlockingLoaderLock, held.loaderCookie);
    }
    held.loaderCookie = 0;
}

/** @param held Suspended threads to resume and release. */
void resume_threads(Held& held) noexcept {
    for (std::size_t index = 0; index < held.count; ++index) {
        (void)ResumeThread(held.handles[index]);
        CloseHandle(held.handles[index]);
    }
    held.handles = {};
    held.ids = {};
    held.count = 0;
}

/**
 * Checks whether one thread id was already held by an earlier snapshot.
 * @param held Threads kept suspended so far.
 * @param threadId Candidate process thread id.
 * @return True when the thread is already held.
 */
[[nodiscard]] bool contains(const Held& held, DWORD threadId) noexcept {
    for (std::size_t index = 0; index < held.count; ++index) {
        if (held.ids[index] == threadId) {
            return true;
        }
    }
    return false;
}

/**
 * Suspends one thread and keeps it only when it stopped outside Sunrise code.
 * A thread stopped inside Sunrise may own a Sunrise lock the caller takes next.
 * @param held Receives the handle when the thread is kept.
 * @param range Sunrise image range.
 * @param threadId Process thread to suspend.
 * @return True when the thread is held, or had already exited.
 */
[[nodiscard]] bool
hold_thread(Held& held, const diagnostics::ModuleRange& range, DWORD threadId) noexcept {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threadId);
    if (thread == nullptr) {
        // A thread that exited since the snapshot is absent from the next one.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    if (SuspendThread(thread) == kSuspendFailed) {
        CloseHandle(thread);
        return false;
    }

    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(thread, &context) == FALSE || diagnostics::contains(range, context.Rip)) {
        (void)ResumeThread(thread);
        CloseHandle(thread);
        return false;
    }
    held.handles[held.count] = thread;
    held.ids[held.count] = threadId;
    ++held.count;
    return true;
}

/**
 * Suspends every unseen thread in one process snapshot.
 * @param held Receives handles that stay suspended.
 * @param range Sunrise image range.
 * @param foundUnseen Receives true when this pass saw a new thread id.
 * @return True when the complete snapshot was held.
 */
[[nodiscard]] bool
hold_snapshot(Held& held, const diagnostics::ModuleRange& range, bool& foundUnseen) noexcept {
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
        const bool needsHold =
            entry.th32ThreadID != currentThreadId && !contains(held, entry.th32ThreadID);
        if (belongsToProcess && needsHold) {
            foundUnseen = true;
            succeeded =
                held.count < held.handles.size() && hold_thread(held, range, entry.th32ThreadID);
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
 * Suspends new threads until a full snapshot finds none, then tests the log sinks.
 * @param held Receives every held handle.
 * @param range Sunrise image range.
 * @return True when every process thread is held at a safe stop.
 */
[[nodiscard]] bool hold_once(Held& held, const diagnostics::ModuleRange& range) noexcept {
    bool foundUnseen = true;
    while (foundUnseen) {
        if (!hold_snapshot(held, range, foundUnseen)) {
            return false;
        }
        // Earlier handles stay suspended while a later pass finds newly created threads.
    }
    // A held thread inside a sink stopped in the operating system, so no address test sees it.
    return !core::log::writers_active();
}

} // namespace

/** Suspends every other thread in the process. */
bool hold(Held& held) noexcept {
    held = {};
    const diagnostics::ModuleRange range = own_range();
    if (range.end == 0 || loader().lock == nullptr || loader().unlock == nullptr) {
        return false;
    }

    enter_exclusive();
    for (std::size_t attempt = 0; attempt < kAttemptLimit; ++attempt) {
        if (!take_locks(held)) {
            break;
        }
        if (hold_once(held, range)) {
            return true;
        }
        // A retry must run with the process locks free, or a resumed thread cannot make the
        // progress that would let the next pass accept it.
        resume_threads(held);
        drop_locks(held);
        Sleep(kRetryMilliseconds);
    }
    leave_exclusive();
    return false;
}

/** Resumes and releases every held thread. */
void release(Held& held) noexcept {
    // Both locks are taken together, so either flag marks a hold that claimed the process.
    const bool claimed = held.loaderLocked || held.heapLocked;
    resume_threads(held);
    drop_locks(held);
    if (claimed) {
        leave_exclusive();
    }
}

/** Blocks until this thread is the only suspender in the process. */
void enter_exclusive() noexcept {
    AcquireSRWLockExclusive(&g_exclusiveLock);
}

/** Releases the claim taken by enter_exclusive. */
void leave_exclusive() noexcept {
    ReleaseSRWLockExclusive(&g_exclusiveLock);
}

} // namespace sunrise::client::process::freeze
