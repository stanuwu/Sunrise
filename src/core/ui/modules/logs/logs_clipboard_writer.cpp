#include <Windows.h>

#include <array>
#include <cstring>
#include <string_view>

#include "internal.h"

namespace sunrise::core::ui::modules::logs::internal {
namespace {

/** CRLF gives copied lines the native Windows text-file separator. */
constexpr std::string_view kLineEnding = "\r\n";
/** One trailing null byte is required by the CF_TEXT clipboard contract. */
constexpr std::size_t kClipboardTerminatorBytes = 1;
/** CF_TEXT matches the structured logger's ASCII event grammar without conversion. */
constexpr UINT kClipboardFormat = CF_TEXT;
/** GMEM_MOVEABLE is required when clipboard ownership crosses the Win32 API boundary. */
constexpr UINT kClipboardMemoryFlags = GMEM_MOVEABLE;
/** The ring limit caps one whole clipboard payload, separators and null included. */
constexpr std::size_t kMaximumClipboardBytes =
    log::snapshot::kEntryCapacity * (log::kLineCapacity + kLineEnding.size())
    + kClipboardTerminatorBytes;

struct ClipboardState {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<char, kMaximumClipboardBytes> payload{};
    std::size_t payloadBytes{};
    CopyStatus status{CopyStatus::idle};
    bool dispatching{};
    bool cancelDispatchResult{};
};

ClipboardState g_clipboard;

/**
 * Measures the whole CF_TEXT payload without letting the size overflow.
 * @param visible Borrowed selection whose source snapshot stays alive for the call.
 * @param bytes Receives the payload size, including the trailing null byte.
 * @return True when every visible line fits the fixed clipboard limit.
 */
[[nodiscard]] bool measure_payload(const log::view::Result& visible, std::size_t& bytes) noexcept {
    bytes = kClipboardTerminatorBytes;
    for (const log::snapshot::Entry* entry : visible.entries()) {
        const std::size_t lineBytes = entry->text().size() + kLineEnding.size();
        if (lineBytes > kMaximumClipboardBytes - bytes) {
            return false;
        }
        bytes += lineBytes;
    }
    return true;
}

/**
 * Writes all visible lines to one locked movable-memory payload.
 * @param visible Borrowed selection whose source snapshot stays alive for the call.
 * @param destination Locked clipboard memory of the measured size.
 */
void write_payload(const log::view::Result& visible, char* destination) noexcept {
    std::size_t offset = 0;
    for (const log::snapshot::Entry* entry : visible.entries()) {
        const std::string_view text = entry->text();
        if (!text.empty()) {
            std::memcpy(destination + offset, text.data(), text.size());
            offset += text.size();
        }
        std::memcpy(destination + offset, kLineEnding.data(), kLineEnding.size());
        offset += kLineEnding.size();
    }
    destination[offset] = '\0';
}

/** Clears every kept line byte. The clipboard lock must be held for writing. */
void clear_payload_locked() noexcept {
    SecureZeroMemory(g_clipboard.payload.data(), g_clipboard.payload.size());
    g_clipboard.payloadBytes = 0;
}

/**
 * Replaces the Win32 clipboard through a real window owner.
 * @param owner Valid output window receiving clipboard ownership.
 * @param payload Null-terminated CF_TEXT bytes, kept alive for the whole call.
 * @return True only when Win32 accepts ownership of the movable payload.
 */
[[nodiscard]] bool copy_with_windows(HWND owner, std::string_view payload) noexcept {
    if (owner == nullptr || IsWindow(owner) == FALSE || payload.empty() || payload.back() != '\0') {
        return false;
    }

    HGLOBAL memory = GlobalAlloc(kClipboardMemoryFlags, static_cast<SIZE_T>(payload.size()));
    if (memory == nullptr) {
        return false;
    }
    auto* destination = static_cast<char*>(GlobalLock(memory));
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, payload.data(), payload.size());
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    bool transferred = false;
    if (EmptyClipboard()) {
        transferred = SetClipboardData(kClipboardFormat, memory) != nullptr;
    }
    CloseClipboard();
    if (!transferred) {
        GlobalFree(memory);
    }
    return transferred;
}

} // namespace

/** Queues all visible structured log lines without calling Win32 from the render lock. */
bool request_copy(const log::view::Result& visible) noexcept {
    std::size_t payloadBytes = 0;
    if (!measure_payload(visible, payloadBytes) || payloadBytes > kMaximumClipboardBytes) {
        return false;
    }

    AcquireSRWLockExclusive(&g_clipboard.lock);
    if (g_clipboard.dispatching) {
        ReleaseSRWLockExclusive(&g_clipboard.lock);
        return false;
    }
    clear_payload_locked();
    write_payload(visible, g_clipboard.payload.data());
    g_clipboard.payloadBytes = payloadBytes;
    g_clipboard.status = CopyStatus::pending;
    g_clipboard.cancelDispatchResult = false;
    ReleaseSRWLockExclusive(&g_clipboard.lock);
    return true;
}

/** @return State of the last explicit clipboard request, read under the lock. */
CopyStatus copy_status() noexcept {
    AcquireSRWLockShared(&g_clipboard.lock);
    const CopyStatus status = g_clipboard.status;
    ReleaseSRWLockShared(&g_clipboard.lock);
    return status;
}

/** Runs one pending payload through a caller-supplied bridge, after render locks unwind. */
bool dispatch_pending_copy(HWND owner, CopyAction action) noexcept {
    AcquireSRWLockExclusive(&g_clipboard.lock);
    if (g_clipboard.status != CopyStatus::pending || g_clipboard.dispatching) {
        ReleaseSRWLockExclusive(&g_clipboard.lock);
        return false;
    }
    if (owner == nullptr || IsWindow(owner) == FALSE || action == nullptr) {
        clear_payload_locked();
        g_clipboard.status = CopyStatus::failed;
        ReleaseSRWLockExclusive(&g_clipboard.lock);
        return false;
    }

    g_clipboard.dispatching = true;
    g_clipboard.cancelDispatchResult = false;
    const std::string_view payload(g_clipboard.payload.data(), g_clipboard.payloadBytes);
    ReleaseSRWLockExclusive(&g_clipboard.lock);

    const bool transferred = action(owner, payload);

    AcquireSRWLockExclusive(&g_clipboard.lock);
    if (!g_clipboard.cancelDispatchResult) {
        g_clipboard.status = transferred ? CopyStatus::copied : CopyStatus::failed;
    }
    g_clipboard.dispatching = false;
    g_clipboard.cancelDispatchResult = false;
    clear_payload_locked();
    ReleaseSRWLockExclusive(&g_clipboard.lock);
    return transferred;
}

/** Runs one pending payload through the Win32 clipboard bridge. */
void dispatch_pending_copy(HWND owner) noexcept {
    (void)dispatch_pending_copy(owner, &copy_with_windows);
}

/** Cancels pending work and clears the kept clipboard bytes at a UI lifecycle boundary. */
void cancel_pending_copy() noexcept {
    AcquireSRWLockExclusive(&g_clipboard.lock);
    g_clipboard.status = CopyStatus::idle;
    if (g_clipboard.dispatching) {
        // The bridge owns the buffer until it returns; only its stale result is dropped.
        g_clipboard.cancelDispatchResult = true;
    } else {
        clear_payload_locked();
    }
    ReleaseSRWLockExclusive(&g_clipboard.lock);
}

} // namespace sunrise::core::ui::modules::logs::internal
