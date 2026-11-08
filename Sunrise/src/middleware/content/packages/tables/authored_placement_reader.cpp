#include "authored_placement_reader.h"

#include <bit>
#include <cmath>
#include <limits>

#include "../reader/container_framing.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {

/** Reads the flags byte and the gameworld byte of one placement entry into one raw word. */
bool authored_placement_flags(std::span<const std::byte> blob,
                              std::size_t entry,
                              std::uint32_t& raw) noexcept {
    raw = 0;
    std::uint8_t flags = 0;
    std::uint8_t gameworld = 0;
    if (!read(blob, entry + kAuthoredPlacementFlagsOffset, flags)
        || !read(blob, entry + kAuthoredPlacementGameworldOffset, gameworld)) {
        return false;
    }
    raw = flags | (std::uint32_t{gameworld} << kAuthoredPlacementGameworldShift);
    return true;
}

/** Finds and validates an authored placement array. */
bool authored_placements(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    // A list with no placements is ordinary authored data, so an empty array is not a refusal.
    if (!find_optional_array_at(blob, kAuthoredPlacementDescriptor, output)) {
        return false;
    }
    if (output.count != 0 && output.elementClass != kAuthoredPlacementClass) {
        output = {};
        return false;
    }
    return true;
}

/** Reads one transform from a validated authored placement array. */
bool authored_placement_at(std::span<const std::byte> blob,
                           const Array& array,
                           std::size_t index,
                           AuthoredPlacement& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (!element_offset(array.dataOffset, array.count, kAuthoredPlacementStride, index, offset)
        || !read(blob, offset + kAuthoredPlacementClassListOffset, output.classListTag)) {
        return false;
    }
    output.sourceOffset = offset;
    for (std::size_t lane = 0; lane < output.rotation.size(); ++lane) {
        if (!read(blob,
                  offset + kAuthoredPlacementRotationOffset + lane * sizeof(float),
                  output.rotationBits[lane])) {
            output = {};
            return false;
        }
        output.rotation[lane] = std::bit_cast<float>(output.rotationBits[lane]);
        if (!std::isfinite(output.rotation[lane])) {
            output = {};
            return false;
        }
    }
    for (std::size_t lane = 0; lane < output.position.size(); ++lane) {
        if (!read(blob,
                  offset + kAuthoredPlacementPositionOffset + lane * sizeof(float),
                  output.positionBits[lane])) {
            output = {};
            return false;
        }
        output.position[lane] = std::bit_cast<float>(output.positionBits[lane]);
        if (!std::isfinite(output.position[lane])) {
            output = {};
            return false;
        }
    }
    if (!read(blob, offset + kAuthoredPlacementScaleOffset, output.uniformScaleBits)
        || !read(blob, offset + kAuthoredPlacementNameHashOffset, output.nameHash)
        || !authored_placement_flags(blob, offset, output.placementFlagsRaw)
        || !read(blob, offset + kAuthoredPlacementIdentifierOffset, output.placementIdentifier)
        || !read(blob, offset + kAuthoredPlacementAuxiliaryOffset, output.auxiliaryRelative)) {
        output = {};
        return false;
    }
    output.uniformScale = std::bit_cast<float>(output.uniformScaleBits);
    if (!std::isfinite(output.uniformScale)) {
        output = {};
        return false;
    }
    return true;
}

/** Reads the opaque placement identifier without assigning it semantics. */
bool authored_placement_identifier_at(std::span<const std::byte> blob,
                                      const Array& array,
                                      std::size_t index,
                                      std::uint64_t& output) noexcept {
    output = 0;
    if (array.elementClass != kAuthoredPlacementClass) {
        return false;
    }
    std::size_t offset = 0;
    return element_offset(array.dataOffset, array.count, kAuthoredPlacementStride, index, offset)
           && read(blob, offset + kAuthoredPlacementIdentifierOffset, output);
}

/** Reads the direct resource tag from the bare form of an indirect placement leaf. */
bool authored_placement_direct_tag(std::span<const std::byte> blob,
                                   std::uint32_t leafClass,
                                   std::uint32_t& tag) noexcept {
    tag = 0;
    return leafClass == kAuthoredPlacementLeafClass
           && read(blob, kAuthoredPlacementDirectTagOffset, tag)
           && package_of(tag) != kAbsentPackageId;
}

/** Reads the parent tag from one proved self-relative auxiliary payload. */
bool authored_placement_auxiliary_parent(std::span<const std::byte> blob,
                                         const Array& array,
                                         std::size_t index,
                                         std::uint32_t& tag) noexcept {
    tag = 0;
    std::size_t entry = 0;
    if (array.elementClass != kAuthoredPlacementClass
        || !element_offset(array.dataOffset, array.count, kAuthoredPlacementStride, index, entry)
        || entry > (std::numeric_limits<std::size_t>::max)() - kAuthoredPlacementAuxiliaryOffset) {
        return false;
    }
    const std::size_t field = entry + kAuthoredPlacementAuxiliaryOffset;
    std::int64_t relative = 0;
    if (!read(blob, field, relative)
        || field > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        return false;
    }
    const std::int64_t base = static_cast<std::int64_t>(field);
    if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
        || (relative < 0 && base < (std::numeric_limits<std::int64_t>::min)() - relative)) {
        return false;
    }
    const std::int64_t target = base + relative;
    if (target < static_cast<std::int64_t>(sizeof(std::uint32_t))) {
        return false;
    }
    const auto payload = static_cast<std::size_t>(target);
    std::uint32_t marker = 0;
    return reader::layout::class_word_before(blob, payload, marker)
           && marker == kStaticSpatialAuxiliaryClass
           && read(blob, payload + kStaticSpatialAuxiliaryParentOffset, tag)
           && package_of(tag) != kAbsentPackageId;
}

/** Reads the candidate-table tag from one exact parent-class record. */
bool static_spatial_parent_table(std::span<const std::byte> blob,
                                 std::uint32_t parentClass,
                                 std::uint32_t& tag) noexcept {
    tag = 0;
    return parentClass == kStaticSpatialParentClass
           && read(blob, kStaticSpatialParentTableOffset, tag)
           && package_of(tag) != kAbsentPackageId;
}

} // namespace sunrise::middleware::content::packages::tables
