#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808095.h"

namespace sunrise::middleware::bap::activity_message::scriptable_auth {

namespace generated = middleware::content::packages::schema::generated;

/** Type-32 toggle Auth: a volume reference followed by a signed state in {-1, 0, 1}. */
inline constexpr std::uint8_t kType32SlotType = 32;
inline constexpr std::uint32_t kType32ComponentClass = generated::C80809556::kClass;
inline constexpr std::uint32_t kType32Schema = generated::C8080955a::kClass;
inline constexpr std::uint8_t kVolumeSlotType = 60;
inline constexpr std::size_t kType32BitCount = 57;
inline constexpr std::size_t kType32ByteCount = 8;
/** The state follows the volume ClientRef as two bits of state plus one; zero is the no-op. */
inline constexpr std::uint8_t kType32StateWidth = 2;
inline constexpr std::uint32_t kType32StateOff = 1;
inline constexpr std::uint32_t kType32StateOn = 2;
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
