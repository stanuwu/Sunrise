#include <Windows.h>

#include <cstring>

#include "../../filesystem/path.h"
#include "../../logging/log.h"
#include "workspace_state.h"

namespace sunrise::core::ui::layout::workspace {
void load(State& state) noexcept {
    std::array<char, kFileCapacity> text{};
    path::Buffer target{};
    if (!path::artifact_file(L"workspace.json", target)) return;
    const HANDLE file = CreateFileW(target.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    DWORD read{};
    const bool ok =
        GetFileSizeEx(file, &size) && size.QuadPart > 0
        && size.QuadPart < static_cast<LONGLONG>(text.size())
        && ReadFile(file, text.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)
        && read == size.QuadPart;
    (void)CloseHandle(file);
    if (ok) (void)decode({text.data(), read}, state);
}
bool save(const State& state) noexcept {
    std::array<char, kFileCapacity> text{};
    path::Buffer target{}, temporary{};
    bool ok = encode(state, text) && path::artifact_file(L"workspace.json", target)
              && path::artifact_file(L"workspace.json.tmp", temporary);
    if (ok) {
        const HANDLE file = CreateFileW(temporary.chars.data(),
                                        GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        ok = file != INVALID_HANDLE_VALUE;
        if (ok) {
            DWORD written{};
            const auto size = static_cast<DWORD>(std::strlen(text.data()));
            ok = WriteFile(file, text.data(), size, &written, nullptr) && written == size;
            if (ok) ok = FlushFileBuffers(file) != FALSE;
            const bool closed = CloseHandle(file) != FALSE;
            ok = ok && closed;
            if (ok)
                ok = MoveFileExW(temporary.chars.data(),
                                 target.chars.data(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                     != FALSE;
        }
    }
    if (!ok) {
        // Report once per process, never every frame or repeatedly for the same drag.
        static bool reported{};
        if (!reported)
            log::write(log::Channel::core, log::Level::warn, "ev=workspace stage=save result=fail");
        reported = true;
    }
    return ok;
}
} // namespace sunrise::core::ui::layout::workspace
