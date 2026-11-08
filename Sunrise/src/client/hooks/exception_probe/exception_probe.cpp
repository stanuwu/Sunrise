/**
 * Names the code behind a crash the game's own handler swallows.
 * A vectored handler sees every exception first. It logs throws from this DLL, faults inside its
 * code and access violations in game code, then lets the exception continue unchanged.
 */

#include "exception_probe.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../../../core/logging/log.h"
#include "../../diagnostics/module_range.h"
#include "../../memory/current_process_memory.h"

namespace sunrise::client::hooks::exception_probe {
namespace {

/** MSVC raises every C++ throw with this code. */
constexpr DWORD kCxxExceptionCode = 0xE06D7363;
/** A C++ throw record carries the magic, the object, the throw info and the thrower's base. */
constexpr DWORD kCxxParameterCount = 4;
/** Reports are capped so a throw inside a loop cannot flood the log. */
constexpr unsigned kMaxReports = 16;
/** Game access violations have their own cap, so they cannot use up this DLL's reports. */
constexpr unsigned kMaxGameReports = 4;
/** An access violation record holds the access kind, then the address it touched. */
constexpr DWORD kAccessParameterCount = 2;
/** Return addresses captured on the faulting thread, and how many share one line. */
constexpr DWORD kFrameLimit = 32;
constexpr std::size_t kFramesPerLine = 8;
/** The decorated type name is copied up to this many bytes. */
constexpr std::size_t kTypeNameCapacity = 96;
/** First handler in the chain, so it runs before any handler that ends the process. */
constexpr ULONG kCallFirst = 1;

/** x64 MSVC throw metadata; every pointer field is an offset from the thrower's image base. */
struct ThrowInfo {
    std::uint32_t attributes;
    std::int32_t unwind;
    std::int32_t forwardCompat;
    std::int32_t catchableTypes;
};
struct CatchableType {
    std::uint32_t properties;
    std::int32_t type;
};
/** A type descriptor's decorated name follows its vtable and spare pointers. */
constexpr std::size_t kTypeDescriptorNameOffset = 16;

PVOID g_handler{};
std::atomic_uint g_reports{0};
std::atomic_uint g_gameReports{0};
diagnostics::ModuleRange g_gameRange{};
diagnostics::ModuleRange g_ownRange{};
thread_local bool t_reporting = false;

/** Formats one address against the game image or this DLL, or as raw hex. */
void format_address(std::uintptr_t value, char* out, std::size_t size) noexcept {
    if (diagnostics::contains(g_ownRange, value)) {
        (void)std::snprintf(
            out, size, "own+0x%llX", static_cast<unsigned long long>(value - g_ownRange.base));
    } else if (diagnostics::contains(g_gameRange, value)) {
        (void)std::snprintf(
            out, size, "exe+0x%llX", static_cast<unsigned long long>(value - g_gameRange.base));
    } else {
        (void)std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(value));
    }
}

/**
 * Reads the most derived type name of a C++ throw.
 * @param record Exception record with the four C++ parameters.
 * @param name Receives the decorated name, or stays empty when the metadata is absent.
 */
void thrown_type(const EXCEPTION_RECORD& record,
                 std::array<char, kTypeNameCapacity>& name) noexcept {
    const auto infoAddress = static_cast<std::uintptr_t>(record.ExceptionInformation[2]);
    const auto base = static_cast<std::uintptr_t>(record.ExceptionInformation[3]);
    ThrowInfo info{};
    std::int32_t count = 0;
    std::int32_t first = 0;
    CatchableType type{};
    // Reads go through a checked copy, because a bad record must not fault the handler.
    if (infoAddress == 0 || base == 0
        || !memory::read_current_process(
            nullptr, infoAddress, std::as_writable_bytes(std::span(&info, 1)))
        || info.catchableTypes == 0
        || !memory::read_current_process(
            nullptr, base + info.catchableTypes, std::as_writable_bytes(std::span(&count, 1)))
        || count <= 0
        || !memory::read_current_process(nullptr,
                                         base + info.catchableTypes + sizeof count,
                                         std::as_writable_bytes(std::span(&first, 1)))
        || !memory::read_current_process(
            nullptr, base + first, std::as_writable_bytes(std::span(&type, 1)))
        || type.type == 0
        || !memory::read_current_process(
            nullptr,
            base + type.type + kTypeDescriptorNameOffset,
            std::as_writable_bytes(std::span(name.data(), name.size() - 1)))) {
        name.fill('\0');
    }
}

/** Logs the return addresses of the faulting thread, several per line. */
void report_frames() noexcept {
    std::array<void*, kFrameLimit> frames{};
    const USHORT captured = RtlCaptureStackBackTrace(0, kFrameLimit, frames.data(), nullptr);
    std::array<char, core::log::kLineCapacity> line{};
    std::size_t offset = 0;
    std::size_t onLine = 0;
    for (USHORT index = 0; index < captured; ++index) {
        if (onLine == 0) {
            const int prefix = std::snprintf(line.data(), line.size(), "ev=probe stage=exception");
            offset = prefix > 0 ? static_cast<std::size_t>(prefix) : 0;
        }
        std::array<char, 48> text{};
        format_address(reinterpret_cast<std::uintptr_t>(frames[index]), text.data(), text.size());
        const int piece = std::snprintf(
            line.data() + offset, line.size() - offset, " f%u=%s", index, text.data());
        if (piece > 0) {
            offset += static_cast<std::size_t>(piece);
        }
        if (++onLine == kFramesPerLine || index + 1 == captured) {
            core::log::write(
                core::log::Channel::client, core::log::Level::warn, {line.data(), offset});
            onLine = 0;
        }
    }
}

/** @return True for a C++ throw from this DLL, or a fault whose address is in its code. */
[[nodiscard]] bool owned(const EXCEPTION_RECORD& record) noexcept {
    if (record.ExceptionCode == kCxxExceptionCode) {
        return record.NumberParameters == kCxxParameterCount
               && static_cast<std::uintptr_t>(record.ExceptionInformation[3]) == g_ownRange.base;
    }
    // A stack overflow leaves too little stack to log on.
    return record.ExceptionCode != EXCEPTION_STACK_OVERFLOW
           && (record.ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0
           && diagnostics::contains(g_ownRange,
                                    reinterpret_cast<std::uintptr_t>(record.ExceptionAddress));
}

/**
 * @return True for an access violation in game code. The game's handler swallows it and can stall
 * the frame for good, so it is the only game fault worth a line.
 */
[[nodiscard]] bool game_fault(const EXCEPTION_RECORD& record) noexcept {
    return record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION
           && record.NumberParameters >= kAccessParameterCount
           && diagnostics::contains(g_gameRange,
                                    reinterpret_cast<std::uintptr_t>(record.ExceptionAddress));
}

/** Logs an owned exception or a game access violation, then passes it on unchanged. */
LONG CALLBACK on_exception(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr || t_reporting) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    const bool own = owned(record);
    if (!own && !game_fault(record)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (own ? g_reports.fetch_add(1, std::memory_order_relaxed) >= kMaxReports
            : g_gameReports.fetch_add(1, std::memory_order_relaxed) >= kMaxGameReports) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    t_reporting = true;
    std::array<char, kTypeNameCapacity> type{};
    if (record.ExceptionCode == kCxxExceptionCode) {
        thrown_type(record, type);
    }
    std::array<char, 48> address{};
    format_address(
        reinterpret_cast<std::uintptr_t>(record.ExceptionAddress), address.data(), address.size());
    // Access is 0 for a read, 1 for a write and 8 for an execute.
    const bool access = record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION
                        && record.NumberParameters >= kAccessParameterCount;
    core::log::writef(
        core::log::Channel::client,
        core::log::Level::warn,
        "ev=probe stage=exception code=0x%08lX at=%s tid=0x%08lX type=%s access=%llu "
        "target=0x%llX",
        record.ExceptionCode,
        address.data(),
        GetCurrentThreadId(),
        type[0] != '\0' ? type.data() : "-",
        access ? static_cast<unsigned long long>(record.ExceptionInformation[0]) : 0ULL,
        access ? static_cast<unsigned long long>(record.ExceptionInformation[1]) : 0ULL);
    report_frames();
    t_reporting = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

/** Adds the handler at the front of the vectored chain. */
bool install() noexcept {
    if (g_handler != nullptr) {
        return true;
    }
    (void)diagnostics::module_range(GetModuleHandleW(nullptr), g_gameRange);
    HMODULE own = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&install),
                           &own)
            == 0
        || !diagnostics::module_range(own, g_ownRange)) {
        return false;
    }
    g_handler = AddVectoredExceptionHandler(kCallFirst, &on_exception);
    if (g_handler == nullptr) {
        return false;
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=probe stage=exception result=installed");
    return true;
}

/** Removes the handler. */
bool uninstall() noexcept {
    if (g_handler == nullptr) {
        return true;
    }
    const bool removed = RemoveVectoredExceptionHandler(g_handler) != 0;
    g_handler = nullptr;
    return removed;
}

} // namespace sunrise::client::hooks::exception_probe
