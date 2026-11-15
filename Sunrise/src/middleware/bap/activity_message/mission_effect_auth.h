#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808095.h"
#include "../../encoding/bit_writer.h"
#include "auth_fields.h"
#include "scriptable_auth_body.h"

// A hop-on sensor attaches one hop-on to every host its Auth selects; selecting none detaches all.
// TODO: write the record selection forms; only the filter form is mapped.

namespace sunrise::middleware::bap::activity_message::mission_effect {

namespace fields = auth_fields;
namespace generated = middleware::content::packages::schema::generated;

// Type 26 uses this schema. The filter form has this fixed body size.
inline constexpr std::uint8_t kSlotType = 26;
inline constexpr std::uint32_t kComponentClass = generated::C8080953f::kClass;
inline constexpr std::uint32_t kSchema = generated::C8080954b::kClass;
inline constexpr std::size_t kBits = 186;
inline constexpr std::size_t kBytes = 24;
/** The revision follows two flags and three unused 32-bit fields. */
inline constexpr std::size_t kRevisionOffset = 2 * fields::kBoolWidth + 3 * 32;
/**
 * Sense root: three signed 32-bit counters, then a flag. The third counter echoes the revision of
 * the Auth the sensor applied, and the flag is set while at least one hop-on is attached.
 */
inline constexpr std::uint32_t kSenseSchema = generated::C8080954a::kClass;
inline constexpr std::uint16_t kSenseCounterCount = 3;
inline constexpr std::uint16_t kSenseRevisionOrdinal = 2;
inline constexpr std::uint16_t kSenseAttachedOrdinal = kSenseCounterCount;

/**
 * Encodes the body that attaches the sensor's hop-on to the hosts one object filter selects, or
 * detaches it from every host.
 * @param filter Type-34 object filter that selects the hosts; ignored when detaching.
 * @param attach False writes no selection, which makes the client drop every hop-on.
 * @param revision Positive. A higher one makes the client rebuild its attachments, and the Sense
 * echoes the one it applied. The Host replaces it with the next one it owns.
 */
[[nodiscard]] inline bool encode(scriptable_auth::Type2LaneClientRef filter,
                                 bool attach,
                                 std::int32_t revision,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    if (revision <= 0 || output.size() < kBytes
        || (attach
            && (filter.slotType != scriptable_auth::kType34SlotType || filter.slotIndex < 0))) {
        return false;
    }
    if (!attach) {
        filter = {};
    }
    encoding::bits::Writer writer(output.first(kBytes));
    const std::array<fields::Field, 6> head{{
        // Clear, so the client keeps attaching as hosts start to match, not only on this apply.
        {0, fields::kBoolWidth},
        // Set, the client attaches nothing new; it removes nothing by itself.
        {attach ? 0U : 1U, fields::kBoolWidth},
        // A command the client runs on every host once per increase; unused.
        {fields::kSigned32Bias, 32},
        // The stack count pushed to each hop-on; unused.
        {fields::kSigned32Bias, 32},
        // Pushes the stack count again; unused.
        {fields::kSigned32Bias, 32},
        {static_cast<std::uint32_t>(revision) + fields::kSigned32Bias, 32},
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
