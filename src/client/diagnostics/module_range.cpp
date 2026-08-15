#include "module_range.h"

#include <cstddef>

namespace sunrise::client::diagnostics {

/** Reads one loaded image's address range from its mapped PE headers. */
bool module_range(HMODULE module, ModuleRange& output) noexcept {
    if (module == nullptr) {
        return false;
    }
    // The headers are reached by walking the module pointer, never by casting its integer value.
    const auto* bytes = reinterpret_cast<const std::byte*>(module);
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    output = ModuleRange{base, base + headers->OptionalHeader.SizeOfImage};
    return true;
}

/** Tests one address against an image range. */
bool contains(const ModuleRange& range, std::uintptr_t address) noexcept {
    return range.end != 0 && address >= range.base && address < range.end;
}

} // namespace sunrise::client::diagnostics
