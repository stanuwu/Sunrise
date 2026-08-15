#include <cstddef>

#include "runtime.h"

namespace sunrise::middleware::compression::oodle {
namespace {

/** The game loads this Oodle runtime before Sunrise generates compressed queuez objects. */
constexpr wchar_t kInstalledModuleName[] = L"oo2core_3_win64.dll";
/** Oodle only accepts a destination size that is a multiple of this step. */
constexpr std::size_t kDecodeStep = 0x4000;

} // namespace

/** Compresses through the already-loaded installed Oodle module. */
bool compress_installed(std::span<const std::byte> input,
                        std::span<std::byte> output,
                        std::size_t& written) noexcept {
    const HMODULE module = GetModuleHandleW(kInstalledModuleName);
    return module != nullptr && compress(module, input, output, written);
}

/** Decompresses one package block, whose real size is not recorded. */
bool decompress_installed_block(std::span<const std::byte> input,
                                std::span<std::byte> output,
                                std::size_t& written) noexcept {
    written = 0;
    const HMODULE module = GetModuleHandleW(kInstalledModuleName);
    if (module == nullptr) {
        return false;
    }
    for (std::size_t size = (output.size() / kDecodeStep) * kDecodeStep; size >= kDecodeStep;
         size -= kDecodeStep) {
        if (decompress(module, input, output.first(size))) {
            written = size;
            return true;
        }
    }
    return false;
}

} // namespace sunrise::middleware::compression::oodle
