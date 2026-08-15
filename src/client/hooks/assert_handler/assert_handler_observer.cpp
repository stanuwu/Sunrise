#include "assert_handler_observer.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"

namespace sunrise::client::hooks::assert_handler {
namespace {

/** The game formats assert text into a buffer of this size, so it bounds ours too. */
constexpr std::size_t kTextCapacity = 1024;
/** One log line carries the message plus its fixed prefix. */
constexpr std::size_t kLineCapacity = 1152;
/** A per-frame assert must not fill the log; later hits are counted only. */
constexpr std::uint32_t kReportLimit = 200;
/** Used when the game's own format string cannot be printed. */
constexpr char kUnformattable[] = "<unformattable>";

std::atomic<std::uint32_t> g_reports{};

/**
 * Records one assert without letting logging failures reach the game.
 * @param code Native first argument, always zero at the observed sites.
 * @param text Already-formatted assert message.
 */
void report(int code, const char* text) noexcept {
    const std::uint32_t seen = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen > kReportLimit) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=assert stage=hit n=%u arg0=%d text=%s", seen, code, text);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::error, {line.data(), length});
    if (seen == kReportLimit) {
        core::log::write(
            core::log::Channel::client, core::log::Level::warn, "ev=assert stage=cap result=ok");
    }
}

/**
 * Replacement assert handler. The sites call this slot as a printf-style callback, and returning
 * without calling the game's own handler is what makes the assert non-fatal: the shipped handler
 * builds a crash ticket, shows a dialog and blocks.
 * @param code Native first argument.
 * @param format Native printf-style format string.
 */
void __cdecl handler_body(int code, const char* format, ...) noexcept {
    std::array<char, kTextCapacity> text{};
    if (format == nullptr) {
        std::memcpy(text.data(), kUnformattable, sizeof kUnformattable);
    } else {
        va_list arguments;
        va_start(arguments, format);
        const int written = std::vsnprintf(text.data(), text.size(), format, arguments);
        va_end(arguments);
        if (written < 0) {
            std::memcpy(text.data(), kUnformattable, sizeof kUnformattable);
        }
    }
    text.back() = '\0';

    // An assert raised by our own logging must not re-enter this body.
    static thread_local bool inside = false;
    if (inside) {
        return;
    }
    inside = true;
    report(code, text.data());
    inside = false;
}

} // namespace

/** @return Address of the internal assert handler body. */
void* handler_entry_point() noexcept {
    return reinterpret_cast<void*>(&handler_body);
}

} // namespace sunrise::client::hooks::assert_handler
