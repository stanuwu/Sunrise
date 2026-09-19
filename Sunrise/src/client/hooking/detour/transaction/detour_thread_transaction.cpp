#include "detour_thread_transaction.h"

#include <Windows.h>

#include <TlHelp32.h>
#include <detours.h>

#include "../../../process/freeze/client_process_freeze.h"

namespace sunrise::client::hooking::detour::transaction {
namespace {

/** 4 protected functions per hook bound the fixed range storage, so no heap is used. */
constexpr std::size_t kProtectedCodeLimit = 64;

/** Access an enlisted thread is opened with. Detours reads and rewrites its context. */
constexpr DWORD kEnlistAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT;
/** Access the walk needs of a thread it only names. Asking for less refuses fewer threads. */
constexpr DWORD kWalkAccess = THREAD_QUERY_LIMITED_INFORMATION;
/** The walk is over. NtGetNextThread reports it as a failure status, so it is checked by value. */
constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001AL);

/**
 * Hands back the next thread of one process, in an order fixed for the length of the walk.
 * Passing a null cursor starts it. The returned handle carries the requested access.
 */
using NextThread = LONG(NTAPI*)(HANDLE process,
                                HANDLE cursor,
                                ACCESS_MASK access,
                                ULONG attributes,
                                ULONG flags,
                                HANDLE* next) noexcept;

/** Resolved walk entry, or null once ntdll has been asked and did not export it. */
NextThread g_nextThread{};
/** Set after the one lookup, so a build without the export is not asked again. */
bool g_nextThreadResolved{false};

/**
 * Finds ntdll's own thread walk, once.
 * Never a function-local static: this runs with other threads suspended, so a guard would deadlock.
 * @return The entry point, or null when ntdll does not export it.
 */
[[nodiscard]] NextThread next_thread_entry() noexcept {
    if (!g_nextThreadResolved) {
        g_nextThreadResolved = true;
        if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
            // The cast is through a void function pointer because GetProcAddress returns FARPROC.
            g_nextThread = reinterpret_cast<NextThread>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtGetNextThread")));
        }
    }
    return g_nextThread;
}

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
    process::freeze::release_process_locks(threads.processLocks);
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

/** How far one enlistment pass got. */
enum class PassResult {
    /** Every thread of the process was seen and taken. */
    complete,
    /** The walk stopped early without handing Detours anything, so another pass may still run. */
    enumerationFailed,
    /** Detours refused a thread. Nothing can continue this transaction. */
    transactionFailed,
};

/**
 * Enlists one process thread by id, unless this transaction already holds it.
 * A thread OpenThread refuses must be left alone: a thread Detours cannot suspend sets a
 * transaction-wide pending error that fails every later attach and that nothing can clear.
 * @param threads Receives the handle, which stays suspended until the transaction ends.
 * @param threadId Candidate process thread id.
 * @param currentThreadId The calling thread, which the transaction enlists separately.
 * @param foundUnseen Set when the thread was new to this transaction.
 * @return False when Detours refused the thread and the transaction is spent.
 */
[[nodiscard]] bool enlist_thread_id(Threads& threads,
                                    DWORD threadId,
                                    DWORD currentThreadId,
                                    bool& foundUnseen) noexcept {
    if (threadId == 0 || threadId == currentThreadId || contains(threads, threadId)) {
        return true;
    }
    foundUnseen = true;
    if (threads.count == threads.handles.size()) {
        return false;
    }
    const HANDLE thread = OpenThread(kEnlistAccess, FALSE, threadId);
    if (thread == nullptr) {
        // A disappearing thread is absent from the next stable pass.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    if (DetourUpdateThread(thread) != NO_ERROR) {
        CloseHandle(thread);
        return false;
    }
    threads.handles[threads.count] = thread;
    threads.ids[threads.count] = threadId;
    ++threads.count;
    return true;
}

/**
 * Says whether a thread is still running.
 * The walk still reaches exited threads and a snapshot does not. Detours suspends on handover,
 * and suspending an exited thread spends the whole transaction, so drop it before offering it.
 * @param thread Handle opened with at least THREAD_QUERY_LIMITED_INFORMATION.
 * @return True only when the thread is confirmed running.
 */
[[nodiscard]] bool thread_is_running(HANDLE thread) noexcept {
    DWORD exitCode = 0;
    return GetExitCodeThread(thread, &exitCode) != FALSE && exitCode == STILL_ACTIVE;
}

/**
 * Enlists every unseen live thread of this process using ntdll's own walk.
 * The walk names and vets each thread; enlisting it then runs on the shared path.
 * @param threads Receives handles that stay suspended until the transaction ends.
 * @param foundUnseen Receives true when this pass saw any new thread.
 * @return How far the pass got.
 */
[[nodiscard]] PassResult enlist_process_walk(Threads& threads, bool& foundUnseen) noexcept {
    const NextThread nextThread = next_thread_entry();
    if (nextThread == nullptr) {
        return PassResult::enumerationFailed;
    }
    const DWORD currentThreadId = GetCurrentThreadId();
    HANDLE cursor = nullptr;
    for (;;) {
        HANDLE next = nullptr;
        const LONG status = nextThread(GetCurrentProcess(), cursor, kWalkAccess, 0, 0, &next);
        // The cursor is only a position in the walk; the transaction never holds it.
        if (cursor != nullptr) {
            CloseHandle(cursor);
        }
        cursor = nullptr;
        if (status == kStatusNoMoreEntries) {
            return PassResult::complete;
        }
        if (status < 0 || next == nullptr) {
            return PassResult::enumerationFailed;
        }
        // The walk's own handle answers both questions, so the enlist handle is only opened for
        // a thread that is going to be offered.
        const DWORD threadId = thread_is_running(next) ? GetThreadId(next) : 0;
        if (!enlist_thread_id(threads, threadId, currentThreadId, foundUnseen)) {
            CloseHandle(next);
            return PassResult::transactionFailed;
        }
        cursor = next;
    }
}

/**
 * Enlists every unseen thread present in one process-wide snapshot.
 * @param threads Receives handles that stay suspended until the transaction ends.
 * @param foundUnseen Receives true when this pass saw any new thread id.
 * @return True when the whole snapshot was inspected without a hard failure.
 */
[[nodiscard]] bool enlist_snapshot(Threads& threads, bool& foundUnseen) noexcept {
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
        if (entry.th32OwnerProcessID == processId) {
            succeeded = enlist_thread_id(threads, entry.th32ThreadID, currentThreadId, foundUnseen);
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
 * Enlists every unseen process thread in one pass, by whichever walk this build has.
 * Both walks dedupe on the thread id, so the snapshot fallback cannot enlist a thread twice.
 * @param threads Receives handles that stay suspended until the transaction ends.
 * @param foundUnseen Receives true when this pass saw any new thread.
 * @return True when the pass completed without a hard failure.
 */
[[nodiscard]] bool enlist_pass(Threads& threads, bool& foundUnseen) noexcept {
    foundUnseen = false;
    const PassResult walked = enlist_process_walk(threads, foundUnseen);
    if (walked == PassResult::complete) {
        return true;
    }
    // A refused thread has already spent the transaction, so no second walk can rescue it. Only
    // a walk that stopped before Detours was told anything falls through to the snapshot.
    if (walked == PassResult::transactionFailed) {
        return false;
    }
    return enlist_snapshot(threads, foundUnseen);
}

/**
 * Enlists new process threads until a full pass finds no unseen thread id.
 * @param threads Receives every handle the transaction holds.
 * @return True when a full pass found no new thread.
 */
[[nodiscard]] bool enlist_until_stable(Threads& threads) noexcept {
    bool foundUnseen{};
    do {
        if (!enlist_pass(threads, foundUnseen)) {
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
    // DetourUpdateThread suspends immediately and allocates a thread record on each call.
    // Own the loader and heap before the first suspension, or a suspended heap owner can
    // block the next allocation forever. The process freezer uses the same lock order.
    process::freeze::enter_exclusive();
    if (!process::freeze::acquire_process_locks(threads.processLocks)) {
        process::freeze::leave_exclusive();
        return false;
    }
    if (DetourTransactionBegin() != NO_ERROR) {
        close_threads(threads);
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
