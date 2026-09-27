#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "../filesystem/path.h"
#include "core/threading/srw_lock.h"
#include "snapshot/internal.h"

namespace sunrise::core::log {
namespace {

/** Stable serialized channel names ordered by Channel. */
constexpr std::array<std::string_view, static_cast<std::size_t>(Channel::count)> kChannelNames{
    "core", "client", "state", "server", "middleware"};
/** Stable serialized severity names; Level::off is never emitted. */
constexpr std::array<std::string_view, 5> kLevelNames{"error", "warn", "info", "debug", "trace"};
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
/** An elapsed line is one event, a duration and an outcome, so it needs less than a full line. */
constexpr std::size_t kElapsedCapacity = 128;
/** Uppercase digits for the hex traces, which are read beside hex dumps of the same bytes. */
constexpr std::string_view kHexDigits = "0123456789ABCDEF";
/** One byte prints as two hex digits, which is the room each appended byte needs. */
constexpr std::size_t kHexDigitsPerByte = 2;
/** Event text stops before the CRLF and the trailing null. */
constexpr std::size_t kEventTextCapacity =
    kLineCapacity - kLineEnding.size() - kLineTerminatorBytes;
/** Longest " t=" field: the key plus a 64-bit millisecond count. */
constexpr std::size_t kStampCapacity = 32;

struct LogState {
    threading::SrwLock lock{};
    std::array<std::atomic<Level>, static_cast<std::size_t>(Channel::count)> levels{};
    HANDLE file{INVALID_HANDLE_VALUE};
    /** Tick the sinks opened on. Every line carries its offset from this, so stalls are visible. */
    ULONGLONG startTick{};
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
    const std::lock_guard lock(g_log.lock);
    // Resetting under the lifetime lock prevents an admitted writer from repopulating stale view.
    snapshot::internal::reset();
    if (g_log.file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
    g_log.initialized = false;
    g_log.debuggerSink = settings.debuggerSink;
    g_log.startTick = GetTickCount64();
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
    return ready;
}

/** Closes the optional sink and clears the bounded in-memory view. */
void shutdown() noexcept {
    const std::lock_guard lock(g_log.lock);

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
    const std::shared_lock lock(g_log.lock);
    const bool admitted = g_log.initialized && enabled(channel, level);
    return admitted;
}

/** Formats and emits one bounded structured event. */
void write(Channel channel, Level level, std::string_view event) noexcept {
    const auto channelIndex = static_cast<std::size_t>(channel);
    const auto levelIndex = static_cast<std::size_t>(level);
    if (channelIndex >= kChannelNames.size() || levelIndex >= kLevelNames.size()) {
        return;
    }

    const std::shared_lock lock(g_log.lock);
    if (!g_log.initialized || !enabled(channel, level)) {
        return;
    }

    std::array<char, kStampCapacity> stamp{};
    const int stamped =
        std::snprintf(stamp.data(),
                      stamp.size(),
                      " t=%llu ",
                      static_cast<unsigned long long>(GetTickCount64() - g_log.startTick));

    std::array<char, kLineCapacity> line{};
    std::size_t length = append(line, 0, kChannelNames[channelIndex], kEventTextCapacity);
    length = append(line, length, " level=", kEventTextCapacity);
    length = append(line, length, kLevelNames[levelIndex], kEventTextCapacity);
    length = append(line,
                    length,
                    stamped > 0 ? std::string_view(stamp.data(), static_cast<std::size_t>(stamped))
                                : std::string_view(" "),
                    kEventTextCapacity);
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
}

/** Formats and emits one structured event when allowed by the channel threshold. */
void writef(Channel channel, Level level, const char* format, ...) noexcept {
    if (format == nullptr || !accepts(channel, level)) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    va_list arguments;
    va_start(arguments, format);
    const int count = std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    if (count <= 0) {
        return;
    }
    const std::size_t length = (std::min)(static_cast<std::size_t>(count), line.size() - 1);
    write(channel, level, std::string_view(line.data(), length));
}

/** Formats and emits one debug event carrying a duration in the ms field. */
void write_elapsed(Channel channel,
                   std::string_view event,
                   unsigned long long startedTick,
                   std::string_view result) noexcept {
    if (!accepts(channel, Level::debug)) {
        return;
    }
    const unsigned long long elapsed = GetTickCount64() - startedTick;
    std::array<char, kElapsedCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "%.*s ms=%llu result=%.*s",
                                      static_cast<int>(event.size()),
                                      event.data(),
                                      elapsed,
                                      static_cast<int>(result.size()),
                                      result.data());
    if (written <= 0) {
        return;
    }
    // snprintf reports the length before truncation, so the emitted view is clamped to the buffer.
    const auto length =
        std::min(static_cast<std::size_t>(written), line.size() - kLineTerminatorBytes);
    write(channel, Level::debug, {line.data(), length});
}

/** Appends bytes as uppercase hex to a line that already holds its key prefix. */
bool append_hex(std::span<char> line,
                std::size_t& length,
                std::span<const std::byte> bytes) noexcept {
    for (const std::byte byte : bytes) {
        if (length + kHexDigitsPerByte >= line.size()) {
            return false;
        }
        const auto value = std::to_integer<unsigned>(byte);
        line[length++] = kHexDigits[(value >> 4U) & 0xFU];
        line[length++] = kHexDigits[value & 0xFU];
    }
    return true;
}

/** @return True while a sink write is in progress. */
bool writers_active() noexcept {
    return g_writers.load(std::memory_order_acquire) != 0;
}

} // namespace sunrise::core::log
