#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808095.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"

// TODO: Map the hop-on predicate and spawn-entry host model before exposing a mission control.

namespace sunrise::middleware::bap::activity_message::mission_effect {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

// Type 26 uses this schema and fixed body size.
inline constexpr std::uint8_t kSlotType = 26;
inline constexpr std::uint32_t kComponentClass = generated::C8080953f::kClass;
inline constexpr std::uint32_t kSchema = generated::C8080954b::kClass;
inline constexpr std::size_t kBits = 186;
inline constexpr std::size_t kBytes = 24;

/** Encodes the existing constrained wire preset without asserting hop-on lifecycle semantics. */
[[nodiscard]] inline bool encode(scriptable_auth::Type2LaneClientRef filter,
                                 bool enabled,
                                 std::int32_t controlWord,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    if (controlWord <= 0 || output.size() < kBytes
        || (enabled
            && (filter.slotType != scriptable_auth::kType34SlotType || filter.slotIndex < 0))) {
        return false;
    }
    if (!enabled) {
        filter = {};
    }
    encoding::bits::Writer writer(output.first(kBytes));
    const std::array<fields::Field, 6> head{{
        {0, fields::kBoolWidth},
        {enabled ? 0U : 1U, fields::kBoolWidth},
        {fields::kSigned32Bias, 32}, // signed zero
        {fields::kSigned32Bias, 32}, // signed zero
        {fields::kSigned32Bias, 32}, // signed zero
        {static_cast<std::uint32_t>(controlWord) + fields::kSigned32Bias, 32},
    }};
    return fields::write_fields(writer, head)
           && writer.write(filter.registryKey, fields::kClientRefKeyWidth)
           && writer.write(static_cast<std::uint32_t>(filter.slotType) + fields::kClientRefTypeBias,
                           fields::kClientRefTypeWidth)
           && writer.write(
               static_cast<std::uint32_t>(static_cast<std::int32_t>(filter.slotIndex)
                                          + static_cast<std::int32_t>(fields::kClientRefIndexBias)),
               fields::kClientRefIndexWidth)
           && writer.write(0, fields::kBoolWidth)
           && fields::finish_exact(writer, kBits, kBytes, written);
}

} // namespace sunrise::middleware::bap::activity_message::mission_effect
