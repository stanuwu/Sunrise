#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

/** Type-32 toggle Auth: a volume reference followed by a signed state in {-1, 0, 1}. */
inline constexpr std::uint8_t kType32SlotType = 32;
inline constexpr std::uint32_t kType32ComponentClass = 0x80809556;
inline constexpr std::uint32_t kType32Schema = 0x8080955A;
inline constexpr std::uint8_t kVolumeSlotType = 60;
inline constexpr std::size_t kType32BitCount = 57;
inline constexpr std::size_t kType32ByteCount = 8;
/** One volume and an explicit on/off state; the no-op state -1 is not representable. */
struct Type32VolumeBody final {
    std::uint32_t registryKey{};
    std::int16_t slotIndex{-1};
    bool active{};
};
/** Encodes one volume toggle into exactly kType32ByteCount bytes. */
[[nodiscard]] bool encode_type32_volume(const Type32VolumeBody& body,
                                        std::span<std::byte> output,
                                        std::size_t& written) noexcept;
/** Decodes one exact volume toggle body. */
[[nodiscard]] bool decode_type32_volume(std::span<const std::byte> input,
                                        std::size_t bitCount,
                                        Type32VolumeBody& body) noexcept;

} // namespace sunrise::middleware::bap::activity_message::scriptable_auth
