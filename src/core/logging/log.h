#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace sunrise::core::log {

/** 1 KiB caps each serialized event, with room for a trailing null byte. */
inline constexpr std::size_t kLineCapacity = 1024;

/** Subsystem owning one structured log event. */
enum class Channel : std::size_t {
    core,
    client,
    state,
    server,
    middleware,
    count,
};

/** Minimum severity accepted by one log channel. */
enum class Level : unsigned char {
    error,
    warn,
    info,
    debug,
    off,
};

/** Process-wide logging thresholds and sink selection. */
struct Settings {
    std::array<Level, static_cast<std::size_t>(Channel::count)> levels{};
    bool debuggerSink{true};
    bool fileSink{false};
};

/** Returns logging defaults with persistent output disabled. */
[[nodiscard]] Settings defaults() noexcept;

/** Starts configured logging sinks relative to the DLL module. */
[[nodiscard]] bool initialize(void* module, const Settings& settings) noexcept;

/** Closes active sinks and clears the bounded in-memory log view. */
void shutdown() noexcept;

/** @return True while the channel threshold admits this severity. */
[[nodiscard]] bool accepts(Channel channel, Level level) noexcept;

/** Emits one structured event when allowed by the channel threshold. */
void write(Channel channel, Level level, std::string_view event) noexcept;

/**
 * Reports whether any thread is inside a sink write.
 * A sink holds an operating system lock this process shares, so a thread suspended there
 * blocks the next writer. Nothing may suspend process threads while this is true.
 * @return True while a sink write is in progress.
 */
[[nodiscard]] bool writers_active() noexcept;

/**
 * Writes one line straight to the debugger, bypassing the sinks and every threshold.
 * Settings are read before the sinks exist, so a boot failure there has no other channel.
 */
void early(std::string_view event) noexcept;

} // namespace sunrise::core::log
