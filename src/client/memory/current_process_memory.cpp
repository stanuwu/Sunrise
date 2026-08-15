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

} // namespace sunrise::client::memory
