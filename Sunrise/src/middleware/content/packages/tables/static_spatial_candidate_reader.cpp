#include "static_spatial_candidate_reader.h"

#include <cstddef>
#include <cstring>

#include "../schema/generated/tag_classes_808071.h"
#include "../schema/generated/tag_classes_808096.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

namespace generated = schema::generated;

/** Range row fields after its leading count. */
constexpr std::size_t kRangeFirstOffset =
    offsetof(generated::SStaticMeshInstanceGroup, firstInstance);
constexpr std::size_t kRangeResourceOffset =
    offsetof(generated::SStaticMeshInstanceGroup, resourceIndex);
constexpr std::size_t kRangeTrailingOffset =
    offsetof(generated::SStaticMeshInstanceGroup, trailing);
/** Opaque transform vec4 lanes; each spans more than one view field. */
constexpr std::size_t kTransformSecondOffset =
    offsetof(generated::SStaticInstanceTransform, position);
constexpr std::size_t kTransformThirdOffset =
    offsetof(generated::SStaticInstanceTransform, lodNearScale);
/** The paired row's second vector. */
constexpr std::size_t kBoundsSecondOffset = offsetof(generated::SInstanceBoundsRow, maximumW1);
/** The opaque tail runs from the visibility id to the row end. */
constexpr std::size_t kBoundsTailOffset = offsetof(generated::SInstanceBoundsRow, visibilityId);
static_assert(sizeof(StaticSpatialBoundsCandidate::opaque)
              == sizeof(generated::SInstanceBoundsRow) - kBoundsTailOffset);

/** @return True when a found array names this element class, or holds no entries. */
[[nodiscard]] bool typed_array(const Array& array, std::uint32_t elementClass) noexcept {
    return array.count == 0 || array.elementClass == elementClass;
}

} // namespace

/** Reads the three typed arrays and paired tag without assigning renderer semantics. */
bool static_spatial_table(std::span<const std::byte> blob, StaticSpatialTable& output) noexcept {
    output = {};
    // Each array may be authored with no entries, and an empty one carries no element class.
    if (!read(blob, kStaticSpatialBoundsTagOffset, output.boundsTag)
        || package_of(output.boundsTag) == kAbsentPackageId
        || !find_optional_array_at(blob, kStaticSpatialTransformDescriptor, output.transforms)
        || !typed_array(output.transforms, kStaticSpatialTransformClass)
        || !find_optional_array_at(blob, kStaticSpatialResourceDescriptor, output.resources)
        || !typed_array(output.resources, kStaticSpatialResourceClass)
        || !find_optional_array_at(blob, kStaticSpatialRangeDescriptor, output.ranges)
        || !typed_array(output.ranges, kStaticSpatialRangeClass)) {
        output = {};
        return false;
    }
    return true;
}

/** Reads one three-vector row from a validated candidate table. */
bool static_spatial_transform_at(std::span<const std::byte> blob,
                                 const Array& transforms,
                                 std::size_t index,
                                 StaticSpatialTransformCandidate& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (transforms.elementClass != kStaticSpatialTransformClass
        || !element_offset(
            transforms.dataOffset, transforms.count, kStaticSpatialTransformStride, index, offset)
        || !read_vector(blob, offset, output.first)
        || !read_vector(blob, offset + kTransformSecondOffset, output.second)
        || !read_vector(blob, offset + kTransformThirdOffset, output.third)) {
        output = {};
        return false;
    }
    return true;
}

/** Reads one resource tag from a validated candidate table. */
bool static_spatial_resource_at(std::span<const std::byte> blob,
                                const Array& resources,
                                std::size_t index,
                                std::uint32_t& output) noexcept {
    output = 0;
    std::size_t offset = 0;
    return resources.elementClass == kStaticSpatialResourceClass
           && element_offset(
               resources.dataOffset, resources.count, kStaticSpatialResourceStride, index, offset)
           && read(blob, offset, output) && package_of(output) != kAbsentPackageId;
}

/** Reads one ordered range row from a validated candidate table. */
bool static_spatial_range_at(std::span<const std::byte> blob,
                             const Array& ranges,
                             std::size_t index,
                             StaticSpatialRange& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (ranges.elementClass != kStaticSpatialRangeClass
        || !element_offset(
            ranges.dataOffset, ranges.count, kStaticSpatialRangeStride, index, offset)
        || !read(blob, offset, output.count)
        || !read(blob, offset + kRangeFirstOffset, output.first)
        || !read(blob, offset + kRangeResourceOffset, output.resourceIndex)
        || !read(blob, offset + kRangeTrailingOffset, output.trailing)) {
        output = {};
        return false;
    }
    return true;
}

/** Finds the typed row array in one paired bounds table. */
bool static_spatial_bounds(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    return find_optional_array_at(blob, kStaticSpatialBoundsDescriptor, output)
           && typed_array(output, kStaticSpatialBoundsClass);
}

/** Reads one two-vector row and opaque tail from a validated paired table. */
bool static_spatial_bounds_at(std::span<const std::byte> blob,
                              const Array& bounds,
                              std::size_t index,
                              StaticSpatialBoundsCandidate& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (bounds.elementClass != kStaticSpatialBoundsClass
        || !element_offset(
            bounds.dataOffset, bounds.count, kStaticSpatialBoundsStride, index, offset)
        || !read_vector(blob, offset, output.first)
        || !read_vector(blob, offset + kBoundsSecondOffset, output.second)
        || offset + kBoundsTailOffset > blob.size()
        || output.opaque.size() > blob.size() - (offset + kBoundsTailOffset)) {
        output = {};
        return false;
    }
    std::memcpy(
        output.opaque.data(), blob.data() + offset + kBoundsTailOffset, output.opaque.size());
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
