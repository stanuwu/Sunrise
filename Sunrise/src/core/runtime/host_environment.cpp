#include "host_environment.h"

#include <Windows.h>

namespace sunrise::core::runtime {
namespace {

/** A zero-duration wait observes process state without blocking the caller. */
constexpr DWORD kNonBlockingWaitMilliseconds = 0;

} // namespace

/** @return True when the loaded ntdll exports Wine's own version entry point. */
bool is_wine() noexcept {
    // Wine exports this from ntdll to name itself and Windows never does. The host cannot change
    // while the process lives, so the answer is resolved once.
    static const bool underWine = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return underWine;
}

/** An access refusal means the process still runs, so only a missing or signaled process counts. */
bool process_stopped(std::uint32_t processId) noexcept {
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }
    const DWORD waitResult = WaitForSingleObject(process, kNonBlockingWaitMilliseconds);
    (void)CloseHandle(process);
    return waitResult == WAIT_OBJECT_0;
}

} // namespace sunrise::core::runtime
