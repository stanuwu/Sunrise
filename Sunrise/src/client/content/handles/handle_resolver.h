#pragma once

#include <cstdint>
#include <span>

namespace sunrise::client::content::handles {

/** Memory-reader callback used by live and synthetic handle sources. */
using Reader = bool (*)(void* context,
                        std::uintptr_t address,
                        std::span<std::byte> output) noexcept;

/** Runtime addresses needed to find one loaded content handle. */
struct Source {
    std::uintptr_t tablesSlot{};
    void* context{};
    Reader read{};
};

/**
 * Finds one loaded content handle without holding on to a game pointer.
 * @param source Bounded runtime-memory source.
 * @param handle Encoded schema or package-content handle.
 * @param address Receives the loaded definition address.
 * @return True when every table field is read and the result is nonzero.
 */
[[nodiscard]] bool
resolve(const Source& source, std::uint32_t handle, std::uintptr_t& address) noexcept;

/** One content tag the client holds in memory. Defrag may move it; resolve again before use. */
struct Resident {
    std::uintptr_t address{};
    /** Bytes the slot owns; at least the package entry's size. */
    std::uint32_t size{};
    std::uint32_t classId{};
};

/**
 * Reads one package content slot.
 * @param handle The tag hash, which is its runtime handle.
 * @param output Receives the slot; zeroed on failure.
 * @return False when the tag is not loaded, still loading, or not an ordinary entry.
 */
[[nodiscard]] bool resident(const Source& source, std::uint32_t handle, Resident& output) noexcept;

} // namespace sunrise::client::content::handles
