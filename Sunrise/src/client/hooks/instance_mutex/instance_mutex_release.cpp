#include "instance_mutex_release.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "../../../core/logging/log.h"
#include "../../hooking/detour/transaction/detour_thread_transaction.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::instance_mutex {
namespace {
// The two named mutexes the game holds for its lifetime as its single-instance guard.
constexpr std::array names{"$ IDA registry mutex $", "$ IDA trusted_idbs"};
using Wait = DWORD(WINAPI*)(HANDLE, DWORD);
using WaitEx = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
SRWLOCK lock = SRWLOCK_INIT;
std::atomic<Wait> original{};
std::atomic<WaitEx> originalEx{};
std::atomic_bool done{};
std::atomic_uint32_t calls{};
void** slot{};
void* restore{};
void* replacement{};

void attempt() noexcept {
    thread_local bool tried = false;
    if (tried || done.load(std::memory_order_acquire)) {
        return;
    }
    tried = true;
    bool released = false;
    for (const char* name : names) {
        HANDLE mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
        if (mutex == nullptr) {
            continue;
        }
        // A new handle can release only acquisitions belonging to this calling thread.
        // The bound only stops a runaway recursion count; stopping early is harmless, because
        // a mutex still held keeps the next launch out rather than corrupting anything.
        constexpr unsigned kReleaseAttempts = 16;
        unsigned count = 0;
        while (count < kReleaseAttempts && ReleaseMutex(mutex)) {
            ++count;
        }
        CloseHandle(mutex);
        released |= count != 0;
    }
    if (released) {
        done.store(true, std::memory_order_release);
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         "ev=instance_mutex result=released");
    }
}

__declspec(noinline) DWORD WINAPI wait(HANDLE handle, DWORD milliseconds) noexcept {
    calls.fetch_add(1, std::memory_order_acq_rel);
    const DWORD error = GetLastError();
    attempt();
    SetLastError(error);
    const DWORD result = original.load(std::memory_order_acquire)(handle, milliseconds);
    calls.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}
__declspec(noinline) DWORD WINAPI wait_ex(HANDLE handle,
                                          DWORD milliseconds,
                                          BOOL alertable) noexcept {
    calls.fetch_add(1, std::memory_order_acq_rel);
    const DWORD error = GetLastError();
    attempt();
    SetLastError(error);
    const DWORD result =
        originalEx.load(std::memory_order_acquire)(handle, milliseconds, alertable);
    calls.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}
bool read_slot(void** location, void*& value) noexcept {
    __try {
        value = *location;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Compare/exchange keeps ownership with a later importer if it already replaced the slot.
bool exchange(void** location, void* expected, void* desired) noexcept {
    DWORD protection{};
    if (!VirtualProtect(location, sizeof(*location), PAGE_READWRITE, &protection)) {
        return false;
    }
    void* previous = InterlockedCompareExchangePointer(location, desired, expected);
    DWORD ignored{};
    if (!VirtualProtect(location, sizeof(*location), protection, &ignored)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=instance_mutex stage=protection result=restore_failed");
    }
    return previous == expected;
}
bool install_locked() noexcept {
    if (slot) {
        return true;
    }
    using namespace patterns;
    // Native wait-result wrapper: distinguish success, abandoned and I/O-completion results.
    constexpr std::string_view text = "48 83 EC 28 48 85 C9 74 21 45 0F B6 C0 FF 15 ? ? ? ? "
                                      "85 C0 74 13 2D 80 00 00 00 74 0C 83 E8 40 74 07";
    constexpr auto pattern = signature<signature_length(text)>(text);
    auto* site = scan_main_image_unique(pattern, "native_wait_import");
    if (!site) {
        return false;
    }
    // CALL [RIP + displacement] names the shared import used by the startup loop. The pair is
    // that displacement's offset in the matched bytes and the next instruction it is relative to.
    auto** location = reinterpret_cast<void**>(resolve_relative(site + 15, site + 19));
    void* current{};
    if (!read_slot(location, current) || !current) {
        return false;
    }
    bool plain = false, extended = false;
    for (const wchar_t* moduleName : {L"kernel32.dll", L"kernelbase.dll"}) {
        const HMODULE module = GetModuleHandleW(moduleName);
        if (!module) {
            continue;
        }
        plain |= current == reinterpret_cast<void*>(GetProcAddress(module, "WaitForSingleObject"));
        extended |=
            current == reinterpret_cast<void*>(GetProcAddress(module, "WaitForSingleObjectEx"));
    }
    if (!plain && !extended) {
        return false;
    }
    void* hook = plain ? reinterpret_cast<void*>(&wait) : reinterpret_cast<void*>(&wait_ex);
    if (plain) {
        original.store(reinterpret_cast<Wait>(current), std::memory_order_release);
    } else {
        originalEx.store(reinterpret_cast<WaitEx>(current), std::memory_order_release);
    }
    if (!exchange(location, current, hook)) {
        return false;
    }
    restore = current;
    replacement = hook;
    slot = location;
    return true;
}
bool uninstall_locked() noexcept {
    if (!slot) {
        return true;
    }
    namespace transaction = hooking::detour::transaction;
    transaction::Threads threads;
    if (!transaction::begin(threads)) {
        return false;
    }
    const std::array entries{
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&wait)},
        hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&wait_ex)}};
    if (transaction::inspect(threads, entries) != transaction::InspectionResult::clear
        || calls.load(std::memory_order_acquire) != 0) {
        (void)transaction::abort(threads);
        return false;
    }
    void* current{};
    bool removed = read_slot(slot, current);
    if (removed && current == replacement) {
        removed = exchange(slot, replacement, restore);
    }
    // No Detours edits were queued. Abort resumes the inspected threads after the IAT edit.
    const bool resumed = transaction::abort(threads);
    if (!removed || !resumed) {
        return false;
    }
    slot = nullptr;
    // Keep native function pointers valid for callers already dispatched through the import.
    return true;
}
} // namespace
bool install() noexcept {
    AcquireSRWLockExclusive(&lock);
    const bool result = install_locked();
    ReleaseSRWLockExclusive(&lock);
    core::log::write(core::log::Channel::client,
                     result ? core::log::Level::info : core::log::Level::warn,
                     result ? "ev=instance_mutex stage=install result=ok"
                            : "ev=instance_mutex stage=install result=fail");
    return result;
}
bool uninstall() noexcept {
    AcquireSRWLockExclusive(&lock);
    const bool result = uninstall_locked();
    ReleaseSRWLockExclusive(&lock);
    return result;
}
void release_once() noexcept {
    attempt();
}
} // namespace sunrise::client::hooks::instance_mutex
