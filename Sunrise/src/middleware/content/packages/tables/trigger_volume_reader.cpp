#include "trigger_volume_reader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

#include "../reader/layout.h"
#include "../reader/typed_array.h"
#include "../schema/generated/tag_classes_808091.h"
#include "internal.h"
#include "slot_descriptor_reader.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

namespace generated = schema::generated;
using generated::C80809b8f;

/** Field offsets of the installed trigger-volume component, root and row layouts. */
constexpr std::size_t kComponentRowHeaderOffset =
    offsetof(generated::PlacedConfigComponentRow, componentHeader.relative);
constexpr std::size_t kComponentRowClassOffset =
    offsetof(generated::PlacedConfigComponentRow, componentClass);
constexpr std::size_t kComponentHeaderOwnTagOffset =
    offsetof(generated::STriggerComponent, ownerConfig.tag);
constexpr std::size_t kComponentHeaderClassOffset =
    offsetof(generated::STriggerComponent, ownerConfig.word);
constexpr std::size_t kComponentHeaderRootOffset =
    offsetof(generated::STriggerComponent, ownerConfig.offset);
constexpr std::size_t kRootKeyOffset = offsetof(generated::STriggerComponentPeer, slotKey);
constexpr std::size_t kRootIdentityOffset = offsetof(generated::STriggerComponentPeer, slotType);
constexpr std::size_t kRootRowsDescriptor =
    offsetof(generated::STriggerComponentPeer, triggerShapes);
constexpr std::size_t kRootRowsRelative =
    offsetof(generated::STriggerComponentPeer, triggerShapes.relative);
constexpr std::size_t kRowKeyOffset = offsetof(generated::STriggerVolumeRow, key);
constexpr std::size_t kRowIdentityOffset = offsetof(generated::STriggerVolumeRow, slotType);
constexpr std::size_t kRowFlagsOffset = offsetof(generated::STriggerVolumeRow, flags);
constexpr std::size_t kRowSpawnEntryOffset = offsetof(generated::STriggerVolumeRow, spawnEntry);
// The spawn entry's position vec4 carries the uniform scale in its w lane.
static_assert(kAuthoredPlacementScaleOffset
              == kAuthoredPlacementPositionOffset
                     + sizeof(generated::AuthoredPlacementEntry::position));
/** Each bounds vec4 carries its w lane, which must be finite too. */
constexpr std::size_t kRowMinimumOffset = offsetof(generated::STriggerVolumeRow, sector.boundsMin);
constexpr std::size_t kRowMaximumOffset = offsetof(generated::STriggerVolumeRow, sector.boundsMax);
static_assert(offsetof(C80809b8f, boundsMinW)
              == offsetof(C80809b8f, boundsMin) + sizeof(C80809b8f::boundsMin));
static_assert(offsetof(C80809b8f, boundsMaxW)
              == offsetof(C80809b8f, boundsMax) + sizeof(C80809b8f::boundsMax));
/** A row's sector shape arrays, its extrusion height and its active flag. */
constexpr std::size_t kRowVertexDescriptor =
    offsetof(generated::STriggerVolumeRow, sector.vertices);
constexpr std::size_t kRowTriangleDescriptor =
    offsetof(generated::STriggerVolumeRow, sector.triangles);
constexpr std::size_t kRowExtrusionOffset =
    offsetof(generated::STriggerVolumeRow, sector.extrusionHeight);
constexpr std::size_t kRowActiveOffset = offsetof(generated::STriggerVolumeRow, active);
/** Auxiliary links a walk follows before it treats the chain as a loop. */
constexpr std::size_t kAuxiliaryChainCapacity = 32;
/** Field offsets of the installed shape-reference layout. */
constexpr std::size_t kShapeResourceOffset = offsetof(generated::TriggerShapeReference, resource);
constexpr std::size_t kShapeReferenceWordOffset =
    offsetof(generated::TriggerShapeReference, referenceWord);
constexpr std::size_t kShapeIndexOffset = offsetof(generated::TriggerShapeReference, shapeIndex);
constexpr std::size_t kShapeKeyOffset = offsetof(generated::TriggerShapeReference, key);
constexpr std::size_t kShapeIdentityOffset =
    offsetof(generated::TriggerShapeReference, identityType);
/** A packed identity is {u8 type, u8 zero, u16 index}; the root, row and shape layouts agree. */
using generated::STriggerComponentPeer;
using generated::STriggerVolumeRow;
using generated::TriggerShapeReference;
// Tag layout: identity fields, measured from the row's slot type.
constexpr std::size_t kIdentityPaddingOffset =
    offsetof(STriggerVolumeRow, gap11) - offsetof(STriggerVolumeRow, slotType);
constexpr std::size_t kIdentityIndexOffset =
    offsetof(STriggerVolumeRow, slotOrdinal) - offsetof(STriggerVolumeRow, slotType);
static_assert(offsetof(STriggerComponentPeer, gap1D) - offsetof(STriggerComponentPeer, slotType)
              == kIdentityPaddingOffset);
static_assert(offsetof(STriggerComponentPeer, slotIndex) - offsetof(STriggerComponentPeer, slotType)
              == kIdentityIndexOffset);
static_assert(offsetof(TriggerShapeReference, gap21) - offsetof(TriggerShapeReference, identityType)
              == kIdentityPaddingOffset);
static_assert(offsetof(TriggerShapeReference, identityIndex)
                  - offsetof(TriggerShapeReference, identityType)
              == kIdentityIndexOffset);

/** Decodes the packed `{u8 type, u8 zero, u16 index}` identity. */
[[nodiscard]] bool read_identity(std::span<const std::byte> blob,
                                 std::size_t offset,
                                 TriggerVolumeIdentity& output) noexcept {
    output = {};
    std::uint8_t padding = 0;
    return read(blob, offset, output.type) && output.type != 0
           && read(blob, offset + kIdentityPaddingOffset, padding) && padding == 0
           && read(blob, offset + kIdentityIndexOffset, output.index);
}

/** Resolves one non-empty typed array descriptor used inside an authored volume row. */
[[nodiscard]] bool row_array(std::span<const std::byte> blob,
                             std::size_t descriptor,
                             std::size_t capacity,
                             std::uint32_t elementClass,
                             std::size_t stride,
                             Array& output) noexcept {
    output = {};
    std::int64_t count = 0;
    std::int64_t relative = 0;
    std::size_t header = 0;
    reader::TypedArrayRows rows{};
    if (!read(blob, descriptor, count) || count <= 0 || count > static_cast<std::int64_t>(capacity)
        || !read(blob, descriptor + reader::layout::kArrayFieldRelative, relative)
        || !relative_offset(descriptor + reader::layout::kArrayFieldRelative, relative, header)
        || !reader::read_typed_array(
            blob, header, static_cast<std::uint64_t>(count), elementClass, stride, rows)) {
        return false;
    }
    output = {static_cast<std::uint64_t>(count), rows.data, rows.elementClass};
    return true;
}

/** Follows the bounded SpawnEntry auxiliary chain to its exact native shape reference. */
[[nodiscard]] bool shape_reference(std::span<const std::byte> blob,
                                   std::size_t row,
                                   TriggerVolumeShapeReference& output) noexcept {
    output = {};
    const std::size_t field = row + kRowSpawnEntryOffset + kAuthoredPlacementAuxiliaryOffset;
    std::int64_t relative = 0;
    std::size_t payload = 0;
    if (!read(blob, field, relative) || relative == 0
        || !relative_offset(field, relative, payload)) {
        return false;
    }
    std::array<std::size_t, kAuxiliaryChainCapacity> visited{};
    std::size_t visitedCount = 0;
    while (visitedCount < visited.size()) {
        const auto scanned = static_cast<std::ptrdiff_t>(visitedCount);
        if (std::find(visited.begin(), visited.begin() + scanned, payload)
            != visited.begin() + scanned) {
            return false;
        }
        visited[visitedCount++] = payload;
        std::uint32_t marker = 0;
        if (!reader::layout::class_word_before(blob, payload, marker) || !is_class_id(marker)) {
            return false;
        }
        if (marker == kTriggerVolumeShapeReferenceClass) {
            std::uint32_t key = 0;
            if (!read(blob, payload + kShapeResourceOffset, output.resourceTag)
                || package_of(output.resourceTag) == kAbsentPackageId
                || !read(blob, payload + kShapeReferenceWordOffset, output.referenceWord)
                || !read(blob, payload + kShapeIndexOffset, output.shapeIndex)
                || !read(blob, payload + kShapeKeyOffset, key)
                || !read_identity(blob, payload + kShapeIdentityOffset, output.identity)) {
                output = {};
                return false;
            }
            output.identity.key = key;
            return true;
        }
        if (!read(blob, payload, relative) || relative == 0
            || !relative_offset(payload, relative, payload)) {
            return false;
        }
    }
    return false;
}

/** @return True when all four lanes of one package vector are finite. */
[[nodiscard]] bool finite(const std::array<float, 4>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float lane) { return std::isfinite(lane); });
}

} // namespace

/** Resolves one trigger-volume component row to its typed root. */
bool trigger_volume_root_at(std::span<const std::byte> blob,
                            const Array& components,
                            std::size_t componentIndex,
                            std::uint32_t configTag,
                            TriggerVolumeRoot& output) noexcept {
    output = {};
    std::size_t descriptor = 0;
    if (components.elementClass != kPlacedConfigComponentRowClass
        || !element_offset(components.dataOffset,
                           components.count,
                           kPlacedConfigComponentStride,
                           componentIndex,
                           descriptor)) {
        return false;
    }
    std::int64_t headerRelative = 0;
    std::uint32_t componentClass = 0;
    std::size_t header = 0;
    std::uint32_t ownTag = 0;
    std::uint32_t headerClass = 0;
    std::uint64_t rootOffset = 0;
    if (!read(blob, descriptor + kComponentRowHeaderOffset, headerRelative)
        || !read(blob, descriptor + kComponentRowClassOffset, componentClass)
        || componentClass != kTriggerVolumeComponentClass
        || !relative_offset(descriptor, headerRelative, header)
        || !read(blob, header + kComponentHeaderOwnTagOffset, ownTag) || ownTag != configTag
        || !read(blob, header + kComponentHeaderClassOffset, headerClass)
        || headerClass != kTriggerVolumeComponentHeaderClass
        || !read(blob, header + kComponentHeaderRootOffset, rootOffset)
        || rootOffset > blob.size()) {
        return false;
    }
    const auto root = static_cast<std::size_t>(rootOffset);
    std::int64_t rowCount = 0;
    std::int64_t rowRelative = 0;
    std::uint32_t key = 0;
    if (!read(blob, root + kRootKeyOffset, key)
        || !read_identity(blob, root + kRootIdentityOffset, output.identity)
        || !read(blob, root + kRootRowsDescriptor, rowCount) || rowCount < 0
        || rowCount > static_cast<std::int64_t>(kTriggerVolumeRowCapacity)
        || !read(blob, root + kRootRowsRelative, rowRelative)) {
        output = {};
        return false;
    }
    output.identity.key = key;
    output.rootOffset = static_cast<std::uint32_t>(rootOffset);
    output.componentOrdinal = static_cast<std::uint32_t>(componentIndex);
    if (rowCount == 0) {
        output.rows.elementClass = kTriggerVolumeRowClass;
        return rowRelative == 0;
    }
    std::size_t headerOffset = 0;
    reader::TypedArrayRows rows{};
    if (!relative_offset(root + kRootRowsRelative, rowRelative, headerOffset)
        || !reader::read_typed_array(blob,
                                     headerOffset,
                                     static_cast<std::uint64_t>(rowCount),
                                     kTriggerVolumeRowClass,
                                     kTriggerVolumeRowStride,
                                     rows)) {
        output = {};
        return false;
    }
    output.rows = {static_cast<std::uint64_t>(rowCount), rows.data, rows.elementClass};
    return true;
}

/** Reads only the exact key/type/index identity from one fixed-stride authored row. */
bool trigger_volume_row_identity_at(std::span<const std::byte> blob,
                                    const TriggerVolumeRoot& root,
                                    std::size_t index,
                                    TriggerVolumeIdentity& output) noexcept {
    output = {};
    std::size_t row = 0;
    std::uint32_t key = 0;
    if (root.rows.elementClass != kTriggerVolumeRowClass
        || !element_offset(
            root.rows.dataOffset, root.rows.count, kTriggerVolumeRowStride, index, row)
        || !read(blob, row + kRowKeyOffset, key)
        || !read_identity(blob, row + kRowIdentityOffset, output)) {
        output = {};
        return false;
    }
    output.key = key;
    return true;
}

/** Reads one fixed-stride authored row and its required shape reference. */
bool trigger_volume_row_at(std::span<const std::byte> blob,
                           const TriggerVolumeRoot& root,
                           std::size_t index,
                           TriggerVolumeRow& output) noexcept {
    output = {};
    std::size_t row = 0;
    std::uint32_t key = 0;
    if (root.rows.elementClass != kTriggerVolumeRowClass
        || !element_offset(
            root.rows.dataOffset, root.rows.count, kTriggerVolumeRowStride, index, row)
        || !read(blob, row + kRowKeyOffset, key)
        || !read_identity(blob, row + kRowIdentityOffset, output.identity)
        || !read(blob, row + kRowFlagsOffset, output.flags)
        || !read(blob,
                 row + kRowSpawnEntryOffset + kAuthoredPlacementClassListOffset,
                 output.classDefinitionTag)
        || package_of(output.classDefinitionTag) == kAbsentPackageId
        || !read_vector(
            blob, row + kRowSpawnEntryOffset + kAuthoredPlacementRotationOffset, output.rotation)
        || !read_vector(
            blob, row + kRowSpawnEntryOffset + kAuthoredPlacementPositionOffset, output.position)
        || !read_vector(blob, row + kRowMinimumOffset, output.minimum)
        || !read_vector(blob, row + kRowMaximumOffset, output.maximum)
        || !row_array(blob,
                      row + kRowVertexDescriptor,
                      kTriggerVolumeVertexCapacity,
                      kTriggerVolumeVertexClass,
                      kTriggerVolumeVertexStride,
                      output.vertices)
        || !row_array(blob,
                      row + kRowTriangleDescriptor,
                      kTriggerVolumeTriangleCapacity,
                      kTriggerVolumeTriangleClass,
                      kTriggerVolumeTriangleStride,
                      output.triangles)
        || !read(blob, row + kRowExtrusionOffset, output.extrusion)
        || !read(blob, row + kRowActiveOffset, output.active)
        || !shape_reference(blob, row, output.shape)) {
        output = {};
        return false;
    }
    output.identity.key = key;
    return trigger_volume_row_finite(output);
}

/** Reads one world-coordinate float4 vertex from a validated row array. */
bool trigger_volume_vertex_at(std::span<const std::byte> blob,
                              const Array& vertices,
                              std::size_t index,
                              std::array<float, 4>& output) noexcept {
    output = {};
    std::size_t offset = 0;
    return vertices.elementClass == kTriggerVolumeVertexClass
           && element_offset(
               vertices.dataOffset, vertices.count, kTriggerVolumeVertexStride, index, offset)
           && read_vector(blob, offset, output) && finite(output);
}

/** Reads one tightly packed three-byte triangle from a validated row array. */
bool trigger_volume_triangle_at(std::span<const std::byte> blob,
                                const Array& triangles,
                                std::size_t index,
                                std::array<std::uint8_t, 3>& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (triangles.elementClass != kTriggerVolumeTriangleClass
        || !element_offset(
            triangles.dataOffset, triangles.count, kTriggerVolumeTriangleStride, index, offset)
        || offset > blob.size() || output.size() > blob.size() - offset) {
        return false;
    }
    std::memcpy(output.data(), blob.data() + offset, output.size());
    return true;
}

/** @return True only when every lane used by volume render math is finite and ordered. */
bool trigger_volume_row_finite(const TriggerVolumeRow& row) noexcept {
    if (!finite(row.rotation) || !finite(row.position) || !finite(row.minimum)
        || !finite(row.maximum) || !std::isfinite(row.extrusion) || row.extrusion < 0.0F) {
        return false;
    }
    for (std::size_t lane = 0; lane < 3; ++lane) {
        if (row.minimum[lane] > row.maximum[lane]) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
