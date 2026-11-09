#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace sunrise::middleware::studio {

// --- Editor mappings: Studio creates each one, the game only opens it ---

/** Maps sizeof(View) bytes of an opened editor mapping; null on failure. */
template <typename View> [[nodiscard]] View* map_editor_view(HANDLE mapping) noexcept {
    return static_cast<View*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(View)));
}

/** Maps the editor's view named by format and this process id; null when there is none. */
template <typename View>
[[nodiscard]] View* open_editor_view(const wchar_t* format, HANDLE& mapping) noexcept {
    std::array<wchar_t, 64> name{};
    if (swprintf_s(name.data(), name.size(), format, GetCurrentProcessId()) <= 0) {
        return nullptr;
    }
    mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.data());
    if (mapping == nullptr) {
        return nullptr;
    }
    auto* const view = map_editor_view<View>(mapping);
    if (view == nullptr) {
        CloseHandle(mapping);
        mapping = nullptr;
    }
    return view;
}

/** True when the editor wrote this exact protocol and its own process id. */
template <typename Header>
[[nodiscard]] bool editor_header_ok(const Header& header,
                                    std::uint32_t magic,
                                    std::uint32_t version,
                                    std::size_t bytes) noexcept {
    return header.magic == magic && header.version == version && header.bytes == bytes
           && header.editorPid != 0;
}

/** Unmaps the view, then closes the mapping; both end null. */
template <typename View> void close_editor_view(HANDLE& mapping, View*& view) noexcept {
    if (view != nullptr) {
        UnmapViewOfFile(view);
        view = nullptr;
    }
    if (mapping != nullptr) {
        CloseHandle(mapping);
        mapping = nullptr;
    }
}

/** False once the heartbeat is unset, in the future, or older than timeout. */
[[nodiscard]] inline bool
editor_alive(volatile std::int64_t& heartbeat, std::uint64_t now, std::uint64_t timeout) noexcept {
    const auto beat = static_cast<std::uint64_t>(InterlockedCompareExchange64(&heartbeat, 0, 0));
    return beat != 0 && beat <= now && now - beat <= timeout;
}

} // namespace sunrise::middleware::studio
