#include "current_process_memory.h"

#include <Windows.h>

#include <cstring>

namespace sunrise::client::memory {

/** Copies one range out of this process using the Windows memory API. */
bool read_current_process([[maybe_unused]] void* context,
                          std::uintptr_t address,
                          std::span<std::byte> output) noexcept {
    if (address == 0 || output.empty()) {
        return false;
    }
    SIZE_T copied = 0;
    const void* source = nullptr;
    static_assert(sizeof source == sizeof address);
    // Copy every address bit without an implementation-defined integer conversion.
    std::memcpy(&source, &address, sizeof source);
    return ReadProcessMemory(GetCurrentProcess(), source, output.data(), output.size(), &copied)
               != FALSE
           && copied == output.size();
}

/** A fault inside the copy fails this read only; the caller's buffer may be partly written. */
bool copy_current_process([[maybe_unused]] void* context,
                          std::uintptr_t address,
                          std::span<std::byte> output) noexcept {
    if (address == 0) {
        return false;
    }
    const void* source = nullptr;
    static_assert(sizeof source == sizeof address);
    // Copy every address bit without an implementation-defined integer conversion.
    std::memcpy(&source, &address, sizeof source);
    __try {
        std::memcpy(output.data(), source, output.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** A fault inside the write fails it; the bytes before the fault stay written. */
bool write_current_process(std::uintptr_t address, std::span<const std::byte> bytes) noexcept {
    if (address == 0) {
        return false;
    }
    void* target = nullptr;
    static_assert(sizeof target == sizeof address);
    std::memcpy(&target, &address, sizeof target);
    __try {
        std::memcpy(target, bytes.data(), bytes.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace sunrise::client::memory
