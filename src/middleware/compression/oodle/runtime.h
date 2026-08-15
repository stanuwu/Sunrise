#pragma once

#include <Windows.h>

#include <cstddef>
#include <span>

namespace sunrise::middleware::compression::oodle {

/**
 * Asks one installed Oodle module how big the output buffer must be.
 * @param module Loaded module that owns the compression exports.
 * @param inputSize Uncompressed byte count.
 * @param capacity Receives the needed compressed-buffer size.
 * @return True when the module has the export and accepts the size.
 */
[[nodiscard]] bool
required_capacity(HMODULE module, std::size_t inputSize, std::size_t& capacity) noexcept;

/**
 * Compresses one non-empty buffer as a bare Kraken stream.
 * @param module Loaded module that owns the compression exports.
 * @param input Uncompressed bytes.
 * @param output Caller storage sized by required_capacity.
 * @param written Receives the compressed byte count.
 * @return True when the whole stream fits and compression works.
 */
[[nodiscard]] bool compress(HMODULE module,
                            std::span<const std::byte> input,
                            std::span<std::byte> output,
                            std::size_t& written) noexcept;

/**
 * Decompresses one package block, whose real size is not recorded.
 * Oodle rejects any size that is not a multiple of the decode step, so the request steps down
 * until one is accepted.
 * @param input Compressed block bytes.
 * @param output Destination of one full block.
 * @param written Receives the produced byte count.
 * @return True when one accepted size decompresses.
 */
[[nodiscard]] bool decompress_installed_block(std::span<const std::byte> input,
                                              std::span<std::byte> output,
                                              std::size_t& written) noexcept;

/**
 * Compresses through the already-loaded installed Oodle module.
 * @param input Uncompressed bytes.
 * @param output Caller-owned compressed storage.
 * @param written Receives the compressed byte count.
 * @return True when the process has the expected module and compression works.
 */
[[nodiscard]] bool compress_installed(std::span<const std::byte> input,
                                      std::span<std::byte> output,
                                      std::size_t& written) noexcept;

/**
 * Decompresses a bare stream into an exact caller-chosen size.
 * @param module Loaded module that owns the decompression export.
 * @param input Compressed stream bytes.
 * @param output Exact uncompressed destination.
 * @return True when it fills the whole destination.
 */
[[nodiscard]] bool
decompress(HMODULE module, std::span<const std::byte> input, std::span<std::byte> output) noexcept;

} // namespace sunrise::middleware::compression::oodle
