#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_80804f.h"
#include "slot_descriptor_reader.h"

namespace sunrise::middleware::content::packages::tables {

/** Handle class named by the descriptor that carries the three-channel type-23 state. */
inline constexpr std::uint32_t kType23ComponentClass =
    schema::generated::DeviceSlotHandleType23::kClass;
/** Exact offset of its opaque placement identifier inside the descriptor. */
inline constexpr std::size_t kType23PlacementIdentifierOffset =
    offsetof(schema::generated::DeviceResourceType23, placementIdentifier);

/**
 * Reads the class-specific opaque identifier used for an exact package equality join.
 * This function deliberately does not assign runtime semantics to the identifier.
 */
[[nodiscard]] bool type23_placement_identifier(std::span<const std::byte> blob,
                                               const SlotDescriptor& descriptor,
                                               std::uint64_t& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
