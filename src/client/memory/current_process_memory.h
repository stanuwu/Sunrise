#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::client::memory {

/**
 * Copies one range out of this process using the Windows memory API.
 * @param context Unused callback context.
 * @param address Runtime source address.
 * @param output Caller's destination buffer.
 * @return True when Windows copies every requested byte.
 */
[[nodiscard]] bool
read_current_process(void* context, std::uintptr_t address, std::span<std::byte> output) noexcept;

} // namespace sunrise::client::memory
