#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808095.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"

// Type-20 damage monitor. The body binds the monitor to one type-4 object and carries a revision
// the client echoes with the object's health and shield fractions.

namespace sunrise::middleware::bap::activity_message::damage_monitor {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

// Type 20 uses these component and Auth/Sense schema identities.
inline constexpr std::uint8_t kSlotType = 20;
inline constexpr std::uint32_t kComponentClass = generated::C80809560::kClass;
inline constexpr std::uint32_t kAuthSchema = generated::C80809563::kClass;
inline constexpr std::uint32_t kSenseSchema = generated::C80809562::kClass;
/** The watched object is a type-4 object slot. */
inline constexpr std::uint32_t kTargetSlotType = 4;
inline constexpr std::size_t kBits = fields::kClientRefBits + 32;
inline constexpr std::size_t kBytes = (kBits + 7) / 8;

/**
 * Encodes the monitor body.
 * @param revision Positive revision; a change re-binds the monitor.
 * @param written Receives kBytes.
 */
[[nodiscard]] inline bool encode(std::uint32_t registryKey,
                                 std::uint16_t slotIndex,
                                 std::int32_t revision,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    if (registryKey == 0 || revision <= 0 || slotIndex > fields::kMaximumClientRefIndex
        || output.size() < kBytes) {
        return false;
    }
    encoding::bits::Writer writer(output.first(kBytes));
    return fields::write_client_ref(writer, registryKey, kTargetSlotType, slotIndex)
           && writer.write(static_cast<std::uint32_t>(revision) + fields::kSigned32Bias, 32)
           && fields::finish_exact(writer, kBits, kBytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::damage_monitor
