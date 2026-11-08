#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_80804e.h"
#include "../../content/packages/schema/generated/tag_classes_80804f.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"

// Type-11 music sensor. The body opens with a 128-bit section selection mask, then one optional
// condition ClientRef per section plus a final one that gates the whole sensor. One set bit lets
// the native bank play that section's transition.

namespace sunrise::middleware::bap::activity_message::music_section {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

// Type 11 uses these component and Auth schema identities.
inline constexpr std::uint8_t kSlotType = 11;
inline constexpr std::uint32_t kComponentClass = generated::MusicSensorComponent::kClass;
inline constexpr std::uint32_t kSchema = generated::C80804f58::kClass;
/** The mask spans four 32-bit lanes, one bit per section. */
inline constexpr std::size_t kSectionCount = 128;
inline constexpr std::size_t kMaskLaneCount = 4;
inline constexpr std::uint8_t kMaskLaneWidth = 32;
/** One condition ClientRef per section, then the sensor gate. All are left unset. */
inline constexpr std::size_t kConditionRefCount = kSectionCount + 1;
inline constexpr std::size_t kBits =
    kMaskLaneCount * kMaskLaneWidth + kConditionRefCount * fields::kClientRefBits;
inline constexpr std::size_t kBytes = (kBits + 7) / 8;

/**
 * Encodes the sensor body with at most one section selected.
 * @param section Section index below kSectionCount.
 * @param enabled False clears the mask.
 * @param written Receives kBytes.
 */
[[nodiscard]] inline bool encode(std::uint8_t section,
                                 bool enabled,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    if (section >= kSectionCount || output.size() < kBytes) {
        return false;
    }
    encoding::bits::Writer writer(output.first(kBytes));
    for (std::size_t lane = 0; lane < kMaskLaneCount; ++lane) {
        const bool selected = enabled && section / kMaskLaneWidth == lane;
        const std::uint32_t mask = selected ? std::uint32_t{1} << (section % kMaskLaneWidth) : 0U;
        if (!writer.write(mask, kMaskLaneWidth)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < kConditionRefCount; ++index) {
        if (!fields::write_absent_client_ref(writer)) {
            return false;
        }
    }
    return fields::finish_exact(writer, kBits, kBytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::music_section
