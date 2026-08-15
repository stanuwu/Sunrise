#include "retail_log_enqueue_observer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../targets/game.h"

namespace sunrise::client::hooks::retail_log {
namespace {

using Enqueue = void(__fastcall*)(std::int32_t, const char*) noexcept;
using SetCategoryVerbosity = void(__fastcall*)(std::int32_t, std::uint32_t) noexcept;

/** The game copies exactly this many bytes out of the caller's text buffer. */
constexpr std::size_t kNativeTextSize = 320;
/** Site id the game uses for an unregistered line. */
constexpr std::int32_t kUnregisteredSite = -1;
/** Line storage holds the cleaned text plus its fixed key prefix. */
constexpr std::size_t kEventCapacity = kNativeTextSize + 64;
/** A late config load can reset all category thresholds, so set them again now and then. */
constexpr unsigned kReassertInterval = 512;
/** How many categories the game's own verbosity table holds. */
constexpr std::uint32_t kCategoryCount = 26;
/** 0 is the game's loosest category threshold. A higher value logs less. */
constexpr std::uint32_t kMostVerbose = 0;

thread_local bool g_inObserver{};
volatile LONG g_sinceAssert{};

/**
 * Copies the native text into fixed storage as one printable line.
 * @param text Borrowed native buffer.
 * @param output Receives the cleaned characters.
 * @return Number of characters written.
 */
[[nodiscard]] std::size_t sanitize(const char* text, std::array<char, kNativeTextSize>& output) {
    std::size_t length = 0;
    __try {
        for (; length < kNativeTextSize - 1 && text[length] != '\0'; ++length) {
            const char value = text[length];
            // One line, one event: the native text carries its own line breaks.
            output[length] = value >= ' ' && value != '\x7F' ? value : ' ';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    while (length != 0 && output[length - 1] == ' ') {
        --length;
    }
    return length;
}

/**
 * Writes one captured line.
 * @param siteId Registered site id.
 * @param text Borrowed native buffer.
 */
void capture_line(std::int32_t siteId, const char* text) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::info)) {
        return;
    }
    std::array<char, kNativeTextSize> sanitized{};
    const std::size_t textLength = sanitize(text, sanitized);
    std::array<char, kEventCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=retail site=%d text=%.*s",
                                      siteId,
                                      static_cast<int>(textLength),
                                      sanitized.data());
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
}

/**
 * Mirrors the single funnel every retail log line passes through.
 * @param siteId Registered site id.
 * @param text Native buffer holding the already-formatted line.
 */
__declspec(noinline) void __fastcall enqueue_body(std::int32_t siteId, const char* text) noexcept {
    // The verbosity setter logs through this same funnel; without this it would recurse.
    const bool outer = !g_inObserver;
    g_inObserver = true;
    const auto call = reinterpret_cast<Enqueue>(g_handle.original);
    if (call != nullptr) {
        call(siteId, text);
    }
    if (outer) {
        if (siteId != kUnregisteredSite && text != nullptr) {
            capture_line(siteId, text);
        }
        assert_verbosity();
        g_inObserver = false;
    }
}

} // namespace

/** @return The enqueue observer body itself, with internal linkage. */
void* enqueue_entry_point() noexcept {
    return reinterpret_cast<void*>(&enqueue_body);
}

/**
 * Opens every category in the game's own log table, once we know the block exists. Reaching the
 * enqueue funnel is the proof: without the block the native body returns early.
 */
void assert_verbosity() noexcept {
    // How much the game logs follows the client threshold, so debug is what opens its table.
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)) {
        return;
    }
    if (InterlockedIncrement(&g_sinceAssert) % kReassertInterval != 1) {
        return;
    }
    const auto setter = reinterpret_cast<SetCategoryVerbosity>(
        targets::game::retail_log::get().setCategoryVerbosity);
    if (setter == nullptr) {
        return;
    }
    for (std::uint32_t category = 0; category < kCategoryCount; ++category) {
        setter(static_cast<std::int32_t>(category), kMostVerbose);
    }
}

} // namespace sunrise::client::hooks::retail_log
