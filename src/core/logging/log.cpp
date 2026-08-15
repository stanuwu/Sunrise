#include "log.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>

#include "../filesystem/path.h"
#include "snapshot/internal.h"

namespace sunrise::core::log {
namespace {

/** Stable serialized channel names ordered by Channel. */
constexpr std::array<std::string_view, static_cast<std::size_t>(Channel::count)> kChannelNames{
    "core", "client", "state", "server", "middleware"};
/** Stable serialized severity names; Level::off is never emitted. */
constexpr std::array<std::string_view, 4> kLevelNames{"error", "warn", "info", "debug"};
/** Optional logs are isolated below the shared generated-artifact directory. */
constexpr std::wstring_view kLogDirectorySuffix = L"\\logs";
/** The active log keeps one stable filename across process starts. */
constexpr std::wstring_view kLogFileSuffix = L"\\sunrise.log";
/** Rotation keeps exactly one prior startup log beside the active file. */
constexpr std::wstring_view kPreviousLogSuffix = L".old";
/** CRLF ends every emitted Windows log record. */
constexpr std::string_view kLineEnding = "\r\n";
/** One trailing null byte is kept for the debugger sink. */
constexpr std::size_t kLineTerminatorBytes = 1;
/** Event text stops before the CRLF and the trailing null. */
constexpr std::size_t kEventTextCapacity =
    kLineCapacity - kLineEnding.size() - kLineTerminatorBytes;

struct LogState {
    SRWLOCK lock{SRWLOCK_INIT};
    std::array<std::atomic<Level>, static_cast<std::size_t>(Channel::count)> levels{};
    HANDLE file{INVALID_HANDLE_VALUE};
    bool debuggerSink{};
    bool initialized{};
};

LogState g_log;
/** Threads inside a sink write. Read by anything that suspends process threads. */
std::atomic_int g_writers{};

/**
 * Tests one event against its channel threshold.
 * @param channel Channel that owns the event.
 * @param level Event severity.
 * @return True when a valid channel threshold permits the event level.
 */
[[nodiscard]] bool enabled(Channel channel, Level level) noexcept {
    const auto index = static_cast<std::size_t>(channel);
    if (index >= g_log.levels.size() || level == Level::off) {
        return false;
    }
    return static_cast<unsigned char>(level)
           <= static_cast<unsigned char>(g_log.levels[index].load(std::memory_order_relaxed));
}

/**
 * Appends as much text as fits before one caller-provided content boundary.
 * @param line Fixed event-line storage.
 * @param offset First free byte.
 * @param text Text to append.
 * @param limit Exclusive content boundary reserved by the caller.
 * @return New first-free byte offset.
 */
[[nodiscard]] std::size_t append(std::array<char, kLineCapacity>& line,
                                 std::size_t offset,
                                 std::string_view text,
                                 std::size_t limit) noexcept {
    const std::size_t available = offset < limit ? limit - offset : 0;
    const std::size_t count = text.size() < available ? text.size() : available;
    if (count != 0) {
        std::memcpy(line.data() + offset, text.data(), count);
    }
    return offset + count;
}

/**
 * Rotates one prior log and opens a new file below the owned artifact directory.
 * @param module Loaded DLL module used to resolve the log directory.
 * @return Writable file handle, or INVALID_HANDLE_VALUE on failure.
 */
[[nodiscard]] HANDLE open_log_file(void* module) noexcept {
    path::Buffer logPath;
    if (!path::artifact_directory(module, logPath) || !path::append(logPath, kLogDirectorySuffix)) {
        return INVALID_HANDLE_VALUE;
    }
    if (!CreateDirectoryW(logPath.chars.data(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        return INVALID_HANDLE_VALUE;
    }

    if (!path::append(logPath, kLogFileSuffix)) {
        return INVALID_HANDLE_VALUE;
    }
    path::Buffer oldPath = logPath;
    if (!path::append(oldPath, kPreviousLogSuffix)) {
        return INVALID_HANDLE_VALUE;
    }
    // Rotation intentionally keeps only one previous file.
    MoveFileExW(logPath.chars.data(), oldPath.chars.data(), MOVEFILE_REPLACE_EXISTING);

    return CreateFileW(logPath.chars.data(),
                       GENERIC_WRITE,
                       FILE_SHARE_READ,
                       nullptr,
                       CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

} // namespace

/** @return Default warning thresholds with debugger output enabled. */
Settings defaults() noexcept {
    Settings settings;
    settings.levels.fill(Level::warn);
    return settings;
}

/** Applies log thresholds and opens the optional file sink. */
bool initialize(void* module, const Settings& settings) noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    // Resetting under the lifetime lock prevents an admitted writer from repopulating stale view.
    snapshot::internal::reset();
    if (g_log.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
    g_log.initialized = false;
    g_log.debuggerSink = settings.debuggerSink;
    for (std::size_t index = 0; index < g_log.levels.size(); ++index) {
        g_log.levels[index].store(settings.levels[index], std::memory_order_relaxed);
    }
    if (settings.fileSink) {
        g_log.file = open_log_file(module);
    }
    const bool ready = !settings.fileSink || g_log.file != INVALID_HANDLE_VALUE;
    g_log.initialized = ready;
    if (!ready) {
        g_log.debuggerSink = false;
        for (std::atomic<Level>& level : g_log.levels) {
            level.store(Level::off, std::memory_order_relaxed);
        }
    }
    ReleaseSRWLockExclusive(&g_log.lock);
    return ready;
}

/** Closes the optional sink and clears the bounded in-memory view. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_log.lock);
    g_log.initialized = false;
    for (std::atomic<Level>& level : g_log.levels) {
        level.store(Level::off, std::memory_order_relaxed);
    }
    g_log.debuggerSink = false;
    if (g_log.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
    // The same lifetime lock excludes writers until both sinks and retained entries are empty.
    snapshot::internal::reset();
    ReleaseSRWLockExclusive(&g_log.lock);
}

/** Writes one line straight to the debugger, bypassing the sinks and every threshold. */
void early(std::string_view event) noexcept {
    std::array<char, kLineCapacity> line{};
    std::size_t length = append(line, 0, "core level=error ", kEventTextCapacity);
    length = append(line, length, event, kEventTextCapacity);
    std::memcpy(line.data() + length, kLineEnding.data(), kLineEnding.size());
    line[length + kLineEnding.size()] = '\0';
    g_writers.fetch_add(1, std::memory_order_acq_rel);
    OutputDebugStringA(line.data());
    g_writers.fetch_sub(1, std::memory_order_acq_rel);
}

/** Reports whether an event would be emitted, so callers can skip the cost of building one. */
bool accepts(Channel channel, Level level) noexcept {
    AcquireSRWLockShared(&g_log.lock);
    const bool admitted = g_log.initialized && enabled(channel, level);
    ReleaseSRWLockShared(&g_log.lock);
    return admitted;
}

/** Formats and emits one bounded structured event. */
void write(Channel channel, Level level, std::string_view event) noexcept {
    const auto channelIndex = static_cast<std::size_t>(channel);
    const auto levelIndex = static_cast<std::size_t>(level);
    if (channelIndex >= kChannelNames.size() || levelIndex >= kLevelNames.size()) {
        return;
    }

    AcquireSRWLockShared(&g_log.lock);
    if (!g_log.initialized || !enabled(channel, level)) {
        ReleaseSRWLockShared(&g_log.lock);
        return;
    }

    std::array<char, kLineCapacity> line{};
    std::size_t length = append(line, 0, kChannelNames[channelIndex], kEventTextCapacity);
    length = append(line, length, " level=", kEventTextCapacity);
    length = append(line, length, kLevelNames[levelIndex], kEventTextCapacity);
    length = append(line, length, " ", kEventTextCapacity);
    length = append(line, length, event, kEventTextCapacity);
    const std::size_t snapshotLength = length;
    std::memcpy(line.data() + length, kLineEnding.data(), kLineEnding.size());
    length += kLineEnding.size();
    line[length] = '\0';

    // The admission lock stays held across the sinks and the snapshot record.
    g_writers.fetch_add(1, std::memory_order_acq_rel);
    if (g_log.debuggerSink) {
        OutputDebugStringA(line.data());
    }
    if (g_log.file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log.file, line.data(), static_cast<DWORD>(length), &written, nullptr);
    }
    g_writers.fetch_sub(1, std::memory_order_acq_rel);
    // Record after sink writes while the shared lifetime lock still excludes shutdown reset.
    snapshot::internal::record(channel, level, std::string_view(line.data(), snapshotLength));
    ReleaseSRWLockShared(&g_log.lock);
}

/** @return True while a sink write is in progress. */
bool writers_active() noexcept {
    return g_writers.load(std::memory_order_acquire) != 0;
}

} // namespace sunrise::core::log
