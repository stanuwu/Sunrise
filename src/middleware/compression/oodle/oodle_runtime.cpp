#include <cstdint>
#include <limits>

#include "runtime.h"

namespace sunrise::middleware::compression::oodle {
namespace {

/** Kraken is the queuez-compatible compressor. Oodle codec value 8. */
constexpr int kKrakenCompressor = 8;
/** Normal level 4 gives the stable runtime stream used for full object snapshots. */
constexpr int kNormalCompressionLevel = 4;
/** Phase 3 matches the queuez reader's one-call, whole-stream decode. */
constexpr int kCompleteThreadPhase = 3;
/** Oodle reports failure with a signed result of zero or less. */
constexpr std::int64_t kFailureResult = 0;
/** The installed version's size query takes only the uncompressed byte count. */
constexpr char kCapacityExport[] = "OodleLZ_GetCompressedBufferSizeNeeded";
/** The compression export makes the bare payload that queuez encoding 4 carries. */
constexpr char kCompressExport[] = "OodleLZ_Compress";
/** The decompression export checks the exact object stride the consumer expects. */
constexpr char kDecompressExport[] = "OodleLZ_Decompress";

using CapacityFunction = std::int64_t(__cdecl*)(std::int64_t inputSize);
using CompressFunction = std::int64_t(__cdecl*)(int compressor,
                                                const void* input,
                                                std::int64_t inputSize,
                                                void* output,
                                                int level,
                                                const void* options,
                                                const void* dictionary,
                                                void* longRangeMatcher,
                                                void* scratch,
                                                std::int64_t scratchSize);
using DecompressFunction = std::int64_t(__cdecl*)(const void* input,
                                                  std::int64_t inputSize,
                                                  void* output,
                                                  std::int64_t outputSize,
                                                  int fuzzSafe,
                                                  int checkCrc,
                                                  std::int64_t verbosity,
                                                  void* decodeBuffer,
                                                  std::int64_t decodeBufferSize,
                                                  void* callback,
                                                  void* callbackContext,
                                                  void* decoderMemory,
                                                  std::int64_t decoderMemorySize,
                                                  int threadPhase);

/** @param size Unsigned size. @return True when the value fits the signed Oodle ABI. */
[[nodiscard]] bool size_fits(std::size_t size) noexcept {
    return size <= static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)());
}

/**
 * Finds one typed export in a loaded module.
 * @tparam Function Exact export function-pointer type.
 * @param module Loaded Windows module.
 * @param name Export name, null-terminated.
 * @return The typed function, or null when the module or export is missing.
 */
template <typename Function>
[[nodiscard]] Function resolve(HMODULE module, const char* name) noexcept {
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

} // namespace

/** Asks the installed codec how big the compressed output can get. */
bool required_capacity(HMODULE module, std::size_t inputSize, std::size_t& capacity) noexcept {
    capacity = 0;
    const CapacityFunction function = resolve<CapacityFunction>(module, kCapacityExport);
    if (function == nullptr || inputSize == 0 || !size_fits(inputSize)) {
        return false;
    }
    const std::int64_t result = function(static_cast<std::int64_t>(inputSize));
    if (result <= kFailureResult
        || static_cast<std::uint64_t>(result)
               > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    capacity = static_cast<std::size_t>(result);
    return true;
}

/** Compresses one fixed-buffer payload. It does not load or keep a module. */
bool compress(HMODULE module,
              std::span<const std::byte> input,
              std::span<std::byte> output,
              std::size_t& written) noexcept {
    written = 0;
    std::size_t required = 0;
    const CompressFunction function = resolve<CompressFunction>(module, kCompressExport);
    if (function == nullptr || !required_capacity(module, input.size(), required)
        || output.size() < required) {
        return false;
    }
    const std::int64_t result = function(kKrakenCompressor,
                                         input.data(),
                                         static_cast<std::int64_t>(input.size()),
                                         output.data(),
                                         kNormalCompressionLevel,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         0);
    if (result <= kFailureResult || static_cast<std::uint64_t>(result) > output.size()) {
        return false;
    }
    written = static_cast<std::size_t>(result);
    return true;
}

/** Decompresses one fixed-buffer payload and rejects partial output. */
bool decompress(HMODULE module,
                std::span<const std::byte> input,
                std::span<std::byte> output) noexcept {
    const DecompressFunction function = resolve<DecompressFunction>(module, kDecompressExport);
    if (function == nullptr || input.empty() || output.empty() || !size_fits(input.size())
        || !size_fits(output.size())) {
        return false;
    }
    const std::int64_t result = function(input.data(),
                                         static_cast<std::int64_t>(input.size()),
                                         output.data(),
                                         static_cast<std::int64_t>(output.size()),
                                         FALSE,
                                         FALSE,
                                         0,
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         0,
                                         kCompleteThreadPhase);
    return result == static_cast<std::int64_t>(output.size());
}

} // namespace sunrise::middleware::compression::oodle
