#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_808000.h"
#include "../schema/generated/tag_classes_808092.h"
#include "../schema/generated/tag_classes_808099.h"
#include "../schema/generated/tag_classes_80809b.h"
#include "../schema/generated/tag_classes_80809f.h"
#include "container_placement_reader.h"

namespace sunrise::middleware::content::packages::tables {

/** Component classes and row classes proved by the package and native shape readers. */
inline constexpr std::uint32_t kTriggerVolumeComponentClass =
    schema::generated::STriggerComponent::kClass;
inline constexpr std::uint32_t kTriggerVolumeComponentHeaderClass =
    schema::generated::STriggerComponentPeer::kClass;
inline constexpr std::uint32_t kTriggerVolumeRowClass =
    schema::generated::STriggerVolumeRow::kClass;
inline constexpr std::uint32_t kTriggerVolumeVertexClass = schema::generated::SVec4Point::kClass;
inline constexpr std::uint32_t kTriggerVolumeTriangleClass =
    schema::generated::TriggerVolumeTriangle::kClass;
inline constexpr std::uint32_t kTriggerVolumeShapeReferenceClass =
    schema::generated::TriggerShapeReference::kClass;

/** Installed row layout and conservative per-record limits. */
inline constexpr std::size_t kTriggerVolumeRowStride = sizeof(schema::generated::STriggerVolumeRow);
inline constexpr std::size_t kTriggerVolumeVertexStride = sizeof(schema::generated::SVec4Point);
inline constexpr std::size_t kTriggerVolumeTriangleStride =
    sizeof(schema::generated::TriggerVolumeTriangle);
inline constexpr std::size_t kTriggerVolumeRowCapacity = 4'096;
inline constexpr std::size_t kTriggerVolumeVertexCapacity = 4'096;
inline constexpr std::size_t kTriggerVolumeTriangleCapacity = 8'192;

/** One exact roster-slot identity carried independently by a root, row, and shape reference. */
struct TriggerVolumeIdentity final {
    std::uint32_t key{};
    std::uint16_t index{};
    std::uint8_t type{};
};

/** One trigger-volume component root and its authored rows. */
struct TriggerVolumeRoot final {
    Array rows{};
    TriggerVolumeIdentity identity{};
    std::uint32_t rootOffset{};
    std::uint32_t componentOrdinal{};
};

/** Native physics-shape resource reference found in the embedded SpawnEntry auxiliary chain. */
struct TriggerVolumeShapeReference final {
    TriggerVolumeIdentity identity{};
    std::uint32_t resourceTag{};
    std::uint32_t referenceWord{};
    std::uint32_t shapeIndex{};
};

/** One package trigger-volume row before its bounded vertex and triangle arrays are copied. */
struct TriggerVolumeRow final {
    TriggerVolumeIdentity identity{};
    TriggerVolumeShapeReference shape{};
    Array vertices{};
    Array triangles{};
    std::uint32_t classDefinitionTag{};
    std::uint32_t flags{};
    std::array<float, 4> rotation{};
    std::array<float, 4> position{};
    std::array<float, 4> minimum{};
    std::array<float, 4> maximum{};
    float extrusion{};
    std::uint8_t active{};
};

/** Resolves one trigger-volume component row to its typed root. */
[[nodiscard]] bool trigger_volume_root_at(std::span<const std::byte> blob,
                                          const Array& components,
                                          std::size_t componentIndex,
                                          std::uint32_t configTag,
                                          TriggerVolumeRoot& output) noexcept;

/** Reads only the exact key/type/index identity from one fixed-stride authored row. */
[[nodiscard]] bool trigger_volume_row_identity_at(std::span<const std::byte> blob,
                                                  const TriggerVolumeRoot& root,
                                                  std::size_t index,
                                                  TriggerVolumeIdentity& output) noexcept;

/** Reads one fixed-stride authored row and its required shape reference. */
[[nodiscard]] bool trigger_volume_row_at(std::span<const std::byte> blob,
                                         const TriggerVolumeRoot& root,
                                         std::size_t index,
                                         TriggerVolumeRow& output) noexcept;

/** Reads one world-coordinate float4 vertex from a validated row array. */
[[nodiscard]] bool trigger_volume_vertex_at(std::span<const std::byte> blob,
                                            const Array& vertices,
                                            std::size_t index,
                                            std::array<float, 4>& output) noexcept;

/** Reads one tightly packed three-byte triangle from a validated row array. */
[[nodiscard]] bool trigger_volume_triangle_at(std::span<const std::byte> blob,
                                              const Array& triangles,
                                              std::size_t index,
                                              std::array<std::uint8_t, 3>& output) noexcept;

/** @return True only when every lane used by volume render math is finite and ordered. */
[[nodiscard]] bool trigger_volume_row_finite(const TriggerVolumeRow& row) noexcept;

} // namespace sunrise::middleware::content::packages::tables
