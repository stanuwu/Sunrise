#include "retail_log_enqueue_observer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../diagnostics/module_range.h"
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
/** A late config load resets the thresholds, so set them again on this period. A count will not
 *  do: a closed category emits fewer lines, so it advances slower and stays closed. */
constexpr std::uint64_t kReassertIntervalMs = 2'000;
/** How many categories the game's own verbosity table holds. */
constexpr std::uint32_t kCategoryCount = 26;
/** 0 is the game's loosest category threshold. A higher value logs less. */
constexpr std::uint32_t kMostVerbose = 0;

thread_local bool g_inObserver{};
/** Tick at which the next re-assert is due. Zero makes the first call assert. */
volatile LONG64 g_nextAssertTick{};
std::atomic_bool g_selectionStackReported{false};
std::atomic_bool g_playerBroadcastStackReported{false};

/** Prefix unique to the real native selection-manager launch event. */
constexpr std::string_view kSelectionLaunchText =
    "world_controller:activity_selection_manager: Launching activity-selection";
/** Prefix emitted at the prologue stall when the local simulation entity cannot be created. */
constexpr std::string_view kPlayerBroadcastFailureText =
    "networking:simulation:entity: failed to create 'player_broadcast' entity";
/** Native code retained before and after each captured return address. */
constexpr std::size_t kSelectionCodeBefore = 0x80;
constexpr std::size_t kSelectionCodeAfter = 0x60;
constexpr std::size_t kSelectionCodeChunk = 16;
/** The nearest game frames contain the selection publication path. */
constexpr unsigned kSelectionCodeFrameLimit = 6;

/** Copies one live code window without letting a bad unwind address escape the observer. */
[[nodiscard]] bool copy_live_code(const void* source, void* output, std::size_t size) noexcept {
    __try {
        CopyMemory(output, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Records live, decrypted instructions around one native selection-manager frame. */
void capture_live_code(const diagnostics::ModuleRange& image,
                       std::string_view stage,
                       unsigned frame,
                       std::uintptr_t returnAddress) noexcept {
    const std::uintptr_t codeStart = returnAddress - image.base >= kSelectionCodeBefore
                                         ? returnAddress - kSelectionCodeBefore
                                         : image.base;
    const std::uintptr_t codeEnd = (std::min)(image.end, returnAddress + kSelectionCodeAfter);
    constexpr char kHex[] = "0123456789ABCDEF";
    for (std::uintptr_t address = codeStart; address < codeEnd; address += kSelectionCodeChunk) {
        const std::size_t count =
            (std::min)(kSelectionCodeChunk, static_cast<std::size_t>(codeEnd - address));
        std::array<unsigned char, kSelectionCodeChunk> bytes{};
        if (!copy_live_code(reinterpret_cast<const void*>(address), bytes.data(), count)) {
            continue;
        }
        std::array<char, kSelectionCodeChunk * 2 + 1> hex{};
        for (std::size_t index = 0; index < count; ++index) {
            hex[index * 2] = kHex[bytes[index] >> 4U];
            hex[index * 2 + 1] = kHex[bytes[index] & 0xFU];
        }
        std::array<char, 192> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=retail stage=%.*s_code frame=%u return_rva=0x%llX code_rva=0x%llX hex=%s",
            static_cast<int>(stage.size()),
            stage.data(),
            frame,
            static_cast<unsigned long long>(returnAddress - image.base),
            static_cast<unsigned long long>(address - image.base),
            hex.data());
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/** Records game-image call frames behind one matching native event once per process. */
void capture_event_stack(std::string_view text,
                         std::string_view prefix,
                         std::string_view stage,
                         std::atomic_bool& reported) noexcept {
    if (!text.starts_with(prefix) || reported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    diagnostics::ModuleRange image{};
    if (!diagnostics::module_range(GetModuleHandleW(nullptr), image)) {
        return;
    }
    std::array<void*, 24> frames{};
    const USHORT count =
        CaptureStackBackTrace(1, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    unsigned codeFrames = 0;
    for (USHORT index = 0; index < count; ++index) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[index]);
        if (!diagnostics::contains(image, address)) {
            continue;
        }
        std::array<char, 128> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=retail stage=%.*s_stack frame=%u image_rva=0x%llX",
                                          static_cast<int>(stage.size()),
                                          stage.data(),
                                          static_cast<unsigned>(index),
                                          static_cast<unsigned long long>(address - image.base));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        if (codeFrames < kSelectionCodeFrameLimit) {
            capture_live_code(image, stage, static_cast<unsigned>(index), address);
            ++codeFrames;
        }
    }
}

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
    const std::string_view cleanText{sanitized.data(), textLength};
    capture_event_stack(cleanText, kSelectionLaunchText, "selection", g_selectionStackReported);
    capture_event_stack(
        cleanText, kPlayerBroadcastFailureText, "player_broadcast", g_playerBroadcastStackReported);
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
    const auto now = static_cast<LONG64>(GetTickCount64());
    const LONG64 due = g_nextAssertTick;
    if (now < due) {
        return;
    }
    // One claim per period, so concurrent funnel threads do not all reopen the table.
    if (InterlockedCompareExchange64(
            &g_nextAssertTick, now + static_cast<LONG64>(kReassertIntervalMs), due)
        != due) {
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
