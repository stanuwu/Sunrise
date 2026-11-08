#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sunrise::state::mods::load_patch {

/** One changed run of a tag; the crc32 guards the bytes it replaces. */
struct Range final {
    std::uint32_t offset{};
    std::uint32_t oldCrc32{};
    std::vector<std::byte> bytes;
};

/** @return The IEEE crc32 of the bytes. */
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

/**
 * Keeps one live update of a tag. The client reloads a tag from the generation it registered at
 * boot, so each later load gets every kept update again, oldest first.
 * @param classId Class the loaded tag must have.
 * @param size Size the loaded tag must have; a kept update never changes it.
 * @param ranges This update's ranges, each guarded against the previous update's bytes.
 * @return False when the update could not be stored.
 */
[[nodiscard]] bool retain(std::uint32_t tag,
                          std::uint32_t classId,
                          std::uint32_t size,
                          std::vector<Range> ranges) noexcept;

/** @return The size the tag's kept updates need, or zero when it has none. */
[[nodiscard]] std::uint32_t kept_size(std::uint32_t tag) noexcept;

/**
 * Writes the kept updates into a tag the client has just loaded, before any class code reads it.
 * Stops at the first update whose guard fails, so the tag is never a mix of two generations.
 * @param blob The loaded tag's slot; it may own more bytes than the entry.
 * @return Updates written.
 */
std::size_t apply(std::uint32_t tag, std::uint32_t classId, std::span<std::byte> blob) noexcept;

} // namespace sunrise::state::mods::load_patch
