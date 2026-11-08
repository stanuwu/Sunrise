#pragma once

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_80806e.h"
#include "../schema/generated/tag_classes_808071.h"
#include "../schema/generated/tag_classes_808094.h"
#include "../schema/generated/tag_classes_808099.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of an authored object-placement list. */
inline constexpr std::uint32_t kAuthoredPlacementListClass =
    schema::generated::SMapPlacements::kClass;
/** Leaf class whose bare form directly names an authored object-placement list. */
inline constexpr std::uint32_t kAuthoredPlacementLeafClass =
    schema::generated::SAuthoredObjectSet::kClass;
/** A placement list holds its entry array descriptor here. */
inline constexpr std::size_t kAuthoredPlacementDescriptor =
    offsetof(schema::generated::SMapPlacements, placements);
/** Element class of a placement-list entry. */
inline constexpr std::uint32_t kAuthoredPlacementClass =
    schema::generated::AuthoredPlacementEntry::kClass;
inline constexpr std::size_t kAuthoredPlacementStride =
    sizeof(schema::generated::AuthoredPlacementEntry);
/** The entry names its placed-object class list here. */
inline constexpr std::size_t kAuthoredPlacementClassListOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, classTag);
/** Quaternion and world-position offsets inside one entry. */
inline constexpr std::size_t kAuthoredPlacementRotationOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, rotation);
inline constexpr std::size_t kAuthoredPlacementPositionOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, position);
inline constexpr std::size_t kAuthoredPlacementScaleOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, uniformScale);
inline constexpr std::size_t kAuthoredPlacementNameHashOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, nameHash);
/** The flags byte, then the gameworld byte. Both are carried in one raw word, flags low. */
inline constexpr std::size_t kAuthoredPlacementFlagsOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, placementFlags);
inline constexpr std::size_t kAuthoredPlacementGameworldOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, gameworldByte);
inline constexpr std::uint32_t kAuthoredPlacementGameworldShift =
    CHAR_BIT * sizeof(schema::generated::AuthoredPlacementEntry::placementFlags);
/** Bit 0 of the flags byte forbids replication whatever the class definition says. */
inline constexpr std::uint32_t kAuthoredPlacementNoReplicationBit = 0x1U;
/** Opaque 64-bit identifier carried by one placement entry. */
inline constexpr std::size_t kAuthoredPlacementIdentifierOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, placementIdentity);
/** One entry's optional self-relative auxiliary payload begins here. */
inline constexpr std::size_t kAuthoredPlacementAuxiliaryOffset =
    offsetof(schema::generated::AuthoredPlacementEntry, payload);
/** Marker carried by the proved auxiliary payload that names a spatial parent. */
inline constexpr std::uint32_t kStaticSpatialAuxiliaryClass =
    schema::generated::SStaticInstancesPayload::kClass;
/** The auxiliary payload names its parent tag here. */
inline constexpr std::size_t kStaticSpatialAuxiliaryParentOffset =
    offsetof(schema::generated::SStaticInstancesPayload, staticOwner);
/** The proved parent record class and its candidate-table tag field. */
inline constexpr std::uint32_t kStaticSpatialParentClass =
    schema::generated::SMapResourceLink::kClass;
inline constexpr std::size_t kStaticSpatialParentTableOffset =
    offsetof(schema::generated::SMapResourceLink, instanceTable);
/** A bare indirect leaf names its resource here. */
inline constexpr std::size_t kAuthoredPlacementDirectTagOffset =
    offsetof(schema::generated::SAuthoredObjectSet, directTarget);

/** One package-authored transform. It does not identify a live ClientRef. */
struct AuthoredPlacement final {
    std::uint32_t classListTag{};
    std::array<float, 4> rotation{};
    std::array<float, 3> position{};
    std::size_t sourceOffset{};
    std::array<std::uint32_t, 4> rotationBits{};
    std::array<std::uint32_t, 3> positionBits{};
    float uniformScale{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::uint64_t placementIdentifier{};
    std::int64_t auxiliaryRelative{};
};

/**
 * Reads the flags byte and the gameworld byte of one placement entry into one raw word.
 * @param blob Bytes holding the entry.
 * @param entry Entry offset.
 * @param raw Receives the flags byte, with the gameworld byte above it.
 * @return True when both bytes are inside the blob.
 */
[[nodiscard]] bool authored_placement_flags(std::span<const std::byte> blob,
                                            std::size_t entry,
                                            std::uint32_t& raw) noexcept;

/** Finds and validates an authored placement array. */
[[nodiscard]] bool authored_placements(std::span<const std::byte> blob, Array& output) noexcept;

/** Reads one transform from a validated authored placement array. */
[[nodiscard]] bool authored_placement_at(std::span<const std::byte> blob,
                                         const Array& array,
                                         std::size_t index,
                                         AuthoredPlacement& output) noexcept;

/** Reads the opaque placement identifier without assigning it semantics. */
[[nodiscard]] bool authored_placement_identifier_at(std::span<const std::byte> blob,
                                                    const Array& array,
                                                    std::size_t index,
                                                    std::uint64_t& output) noexcept;

/** Reads the direct resource tag from the bare form of an indirect placement leaf. */
[[nodiscard]] bool authored_placement_direct_tag(std::span<const std::byte> blob,
                                                 std::uint32_t leafClass,
                                                 std::uint32_t& tag) noexcept;

/** Reads the parent tag from one proved self-relative auxiliary payload. */
[[nodiscard]] bool authored_placement_auxiliary_parent(std::span<const std::byte> blob,
                                                       const Array& array,
                                                       std::size_t index,
                                                       std::uint32_t& tag) noexcept;

/** Reads the candidate-table tag from one exact parent-class record. */
[[nodiscard]] bool static_spatial_parent_table(std::span<const std::byte> blob,
                                               std::uint32_t parentClass,
                                               std::uint32_t& tag) noexcept;

} // namespace sunrise::middleware::content::packages::tables
