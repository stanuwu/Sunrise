#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_808099.h"
#include "authored_placement_reader.h"
#include "slot_descriptor_reader.h"

namespace sunrise::middleware::content::packages::tables {

/** Exact handle class the descriptor names, and the slot type for this family. */
inline constexpr std::uint32_t kDescriptorEmbeddedPlacementDescriptorClass =
    schema::generated::ObjectSlotHandleType4::kClass;
inline constexpr std::uint16_t kDescriptorEmbeddedPlacementSlotType = 4;
/** The descriptor is the slot's definition object; it carries the placed-object array. */
inline constexpr std::size_t kDescriptorEmbeddedPlacementArrayOffset =
    offsetof(schema::generated::ObjectSlotDefinitionType4, embeddedPlacements);

/** One validated native array owned by an exact type-4 slot descriptor. */
struct DescriptorEmbeddedPlacementArray final {
    std::uint64_t count{};
    std::size_t dataOffset{};
    std::uint32_t elementClass{};
};

/** One exact placed-object row embedded in a validated slot descriptor. */
struct DescriptorEmbeddedPlacement final {
    std::uint32_t classListTag{};
    std::uint32_t nameHash{};
    std::uint64_t identifier{};
    std::int64_t auxiliaryRelative{};
    std::size_t auxiliaryOffset{};
    std::array<float, 4> rotation{};
    std::array<float, 3> position{};
    float fourthLane{};
    std::uint8_t replicationByte{};
    std::uint8_t gameworldByte{};
    bool hasAuxiliary{};
};

/** Reads the exact native array descriptor. The zero-count, zero-relative form is valid. */
[[nodiscard]] bool
descriptor_embedded_placements(std::span<const std::byte> blob,
                               const SlotDescriptor& descriptor,
                               DescriptorEmbeddedPlacementArray& output) noexcept;

/** Reads one exact row from a validated descriptor-embedded placement array. */
[[nodiscard]] bool descriptor_embedded_placement_at(std::span<const std::byte> blob,
                                                    const DescriptorEmbeddedPlacementArray& array,
                                                    std::uint64_t index,
                                                    DescriptorEmbeddedPlacement& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
