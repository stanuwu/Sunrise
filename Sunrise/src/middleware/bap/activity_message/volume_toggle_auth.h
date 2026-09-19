#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

/** Build-86657 toggle sensor: an EntryRef followed by signed state {-1,0,1}. */
inline constexpr std::uint8_t kType32SlotType = 32;
inline constexpr std::uint32_t kType32ComponentClass = 0x80809556;
inline constexpr std::uint32_t kType32Schema = 0x8080955A;
inline constexpr std::uint8_t kVolumeSlotType = 60;
inline constexpr std::size_t kType32BitCount = 57;
inline constexpr std::size_t kType32ByteCount = 8;
/** This adapter intentionally supports only a real volume target and an explicit boolean state. */
struct Type32VolumeBody final {
    std::uint32_t registryKey{};
    std::int16_t slotIndex{-1};
    bool active{};
};
[[nodiscard]] bool encode_type32_volume(const Type32VolumeBody& body,
                                        std::span<std::byte> output,
                                        std::size_t& written) noexcept;
[[nodiscard]] bool decode_type32_volume(std::span<const std::byte> input,
                                        std::size_t bitCount,
                                        Type32VolumeBody& body) noexcept;

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
