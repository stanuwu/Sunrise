#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::patch_epoch {

/** Patch-epoch updates use activity message type 52. */
inline constexpr std::uint32_t kMessageType = 52;
/** Two fixed unsigned 64-bit fields are exactly 16 wire bytes. */
inline constexpr std::size_t kEncodedSize = 16;

/** Typed patch epoch echoed by the next authority-only roster update. */
struct PatchEpoch final {
    std::uint64_t first{};
    std::uint64_t second{};
};

/**
 * Parses the fixed 2-field big-endian patch-epoch prefix.
 * @param input Type-52 payload holding both fixed fields.
 * @param epoch Cleared first. Filled in only on success.
 * @return True when the payload holds the whole 16-byte prefix.
 */
[[nodiscard]] bool parse_patch_epoch(std::span<const std::byte> input, PatchEpoch& epoch) noexcept;

} // namespace sunrise::middleware::bap::activity_message::patch_epoch
