#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::datagen::family4::account::selection_patch {

/** The selected-character-only tag-reflection body occupies 129 bits. */
inline constexpr std::size_t kPayloadBitSize = 129;
/** The partial final byte makes the payload 17 bytes. */
inline constexpr std::size_t kPayloadSize = 17;

/**
 * Encodes only the selected-character SOID and its mandatory generated 0.
 * @param selectedSoid Selected character id, or 0 for no selection.
 * @param output Caller-owned tag-reflection payload storage.
 * @param written Receives 17 on success and 0 on failure.
 * @return True when the complete fixed payload fits.
 */
[[nodiscard]] bool
encode(std::uint64_t selectedSoid, std::span<std::byte> output, std::size_t& written) noexcept;

} // namespace sunrise::middleware::datagen::family4::account::selection_patch
