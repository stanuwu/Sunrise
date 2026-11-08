#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_80808a.h"
#include "../schema/generated/tag_classes_80808b.h"
#include "../schema/generated/tag_classes_808091.h"
#include "../schema/generated/tag_classes_808099.h"
#include "authored_placement_reader.h"
#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** A component wraps a spawn set through the payload of its first placement. */
inline constexpr std::uint32_t kComponentSpawnPayloadClass =
    schema::generated::SSpawnPointsPayload::kClass;
inline constexpr std::size_t kComponentSpawnSetOffset =
    offsetof(schema::generated::SSpawnPointsPayload, spawnPoints);
/** Tag class of the container that groups components. */
inline constexpr std::uint32_t kContainerClass = schema::generated::SMapContainer::kClass;
/** A container holds its bubble mask at this offset. */
inline constexpr std::size_t kContainerBubbleMaskOffset =
    offsetof(schema::generated::SMapContainer, bubbleMask);
/** One mask bit per map-global bubble index. */
inline constexpr std::size_t kContainerBubbleMaskBytes =
    sizeof(schema::generated::SMapContainer::bubbleMask);
/** Bubble indices the mask can name. No installed map declares one above 169. */
inline constexpr std::size_t kBubbleIndexCapacity = kContainerBubbleMaskBytes * 8;
/** A container holds its member array descriptor at this offset. */
inline constexpr std::size_t kContainerMemberDescriptor =
    offsetof(schema::generated::SMapContainer, contents);
/** Element class of a container's member array. */
inline constexpr std::uint32_t kContainerMemberClass = schema::generated::ContainerMember::kClass;
/** One member is a bare tag handle. */
inline constexpr std::size_t kContainerMemberStride = sizeof(schema::generated::ContainerMember);

/**
 * Reads the spawn set one component wraps.
 * @param blob Whole component bytes.
 * @param tag Receives the wrapped spawn-set tag.
 * @return True when the first placement carries a spawn payload.
 */
[[nodiscard]] bool component_resource(std::span<const std::byte> blob, std::uint32_t& tag) noexcept;

/**
 * Reads the bubble mask of one container.
 * A set bit is a map-global bubble index the container belongs to. The scenario publishes that
 * index per bubble, so the mask is what binds a container's members to bubbles.
 * @param blob Whole container bytes.
 * @param output Exactly `kContainerBubbleMaskBytes` of caller storage.
 * @return True when the whole mask is inside the blob.
 */
[[nodiscard]] bool container_bubble_mask(std::span<const std::byte> blob,
                                         std::span<std::uint8_t> output) noexcept;

/**
 * Finds the member array of one container.
 * @param blob Whole container bytes.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a member array.
 */
[[nodiscard]] bool container_members(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads one member tag of a container.
 * @param blob Whole container bytes.
 * @param members Member array that was found.
 * @param index Member ordinal.
 * @param tag Receives the member tag.
 * @return True when the ordinal is inside the array.
 */
[[nodiscard]] bool container_member_at(std::span<const std::byte> blob,
                                       const Array& members,
                                       std::uint64_t index,
                                       std::uint32_t& tag) noexcept;

/**
 * Tests one bubble index against a mask.
 * @param mask Bubble mask bytes.
 * @param index Map-global bubble index.
 * @return True when the mask names that bubble.
 */
[[nodiscard]] bool bubble_in_mask(std::span<const std::uint8_t> mask, std::size_t index) noexcept;

} // namespace sunrise::middleware::content::packages::tables
