#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_808071.h"
#include "../schema/generated/tag_classes_808096.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of the package table whose three arrays form one spatial candidate set. */
inline constexpr std::uint32_t kStaticSpatialTableClass =
    schema::generated::SStaticMeshInstances::kClass;
/** Tag class of the paired bounds-candidate table. */
inline constexpr std::uint32_t kStaticSpatialBoundsTableClass =
    schema::generated::SInstanceBoundsTable::kClass;
/** Element classes carried by the four proved array headers. */
inline constexpr std::uint32_t kStaticSpatialTransformClass =
    schema::generated::SStaticInstanceTransform::kClass;
inline constexpr std::uint32_t kStaticSpatialResourceClass =
    schema::generated::SStaticInstanceResource::kClass;
inline constexpr std::uint32_t kStaticSpatialRangeClass =
    schema::generated::SStaticMeshInstanceGroup::kClass;
inline constexpr std::uint32_t kStaticSpatialBoundsClass =
    schema::generated::SInstanceBoundsRow::kClass;
/** Array and link offsets inside the two package records. */
inline constexpr std::size_t kStaticSpatialBoundsTagOffset =
    offsetof(schema::generated::SStaticMeshInstances, boundsTable);
inline constexpr std::size_t kStaticSpatialTransformDescriptor =
    offsetof(schema::generated::SStaticMeshInstances, instances.transforms);
inline constexpr std::size_t kStaticSpatialResourceDescriptor =
    offsetof(schema::generated::SStaticMeshInstances, instances.resources);
inline constexpr std::size_t kStaticSpatialRangeDescriptor =
    offsetof(schema::generated::SStaticMeshInstances, instances.ranges);
inline constexpr std::size_t kStaticSpatialBoundsDescriptor =
    offsetof(schema::generated::SInstanceBoundsTable, boxes);
/** Row strides. */
inline constexpr std::size_t kStaticSpatialTransformStride =
    sizeof(schema::generated::SStaticInstanceTransform);
inline constexpr std::size_t kStaticSpatialResourceStride =
    sizeof(schema::generated::SStaticInstanceResource);
inline constexpr std::size_t kStaticSpatialRangeStride =
    sizeof(schema::generated::SStaticMeshInstanceGroup);
inline constexpr std::size_t kStaticSpatialBoundsStride =
    sizeof(schema::generated::SInstanceBoundsRow);

/** Validated arrays and paired tag from one spatial candidate table. */
struct StaticSpatialTable final {
    std::uint32_t boundsTag{};
    Array transforms{};
    Array resources{};
    Array ranges{};
};

/** Three vec4-shaped fields retained without assigning native transform semantics. */
struct StaticSpatialTransformCandidate final {
    std::array<float, 4> first{};
    std::array<float, 4> second{};
    std::array<float, 4> third{};
};

/** One ordered eight-byte range row. */
struct StaticSpatialRange final {
    std::uint16_t count{};
    std::uint16_t first{};
    std::uint16_t resourceIndex{};
    std::uint16_t trailing{};
};

/** Two vec4-shaped fields and the still-opaque tail of one paired row. */
struct StaticSpatialBoundsCandidate final {
    std::array<float, 4> first{};
    std::array<float, 4> second{};
    std::array<std::byte, 16> opaque{};
};

/** Reads the three typed arrays and paired tag without assigning renderer semantics. */
[[nodiscard]] bool static_spatial_table(std::span<const std::byte> blob,
                                        StaticSpatialTable& output) noexcept;

/** Reads one three-vector row from a validated candidate table. */
[[nodiscard]] bool static_spatial_transform_at(std::span<const std::byte> blob,
                                               const Array& transforms,
                                               std::size_t index,
                                               StaticSpatialTransformCandidate& output) noexcept;

/** Reads one resource tag from a validated candidate table. */
[[nodiscard]] bool static_spatial_resource_at(std::span<const std::byte> blob,
                                              const Array& resources,
                                              std::size_t index,
                                              std::uint32_t& output) noexcept;

/** Reads one ordered range row from a validated candidate table. */
[[nodiscard]] bool static_spatial_range_at(std::span<const std::byte> blob,
                                           const Array& ranges,
                                           std::size_t index,
                                           StaticSpatialRange& output) noexcept;

/** Finds the typed row array in one paired bounds table. */
[[nodiscard]] bool static_spatial_bounds(std::span<const std::byte> blob, Array& output) noexcept;

/** Reads one two-vector row and opaque tail from a validated paired table. */
[[nodiscard]] bool static_spatial_bounds_at(std::span<const std::byte> blob,
                                            const Array& bounds,
                                            std::size_t index,
                                            StaticSpatialBoundsCandidate& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
