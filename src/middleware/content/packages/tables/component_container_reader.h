#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of a component, which wraps one resource so a container can hold it. */
inline constexpr std::uint32_t kComponentClass = 0x808099D6U;
/** A component names the resource it wraps at this offset. */
inline constexpr std::size_t kComponentResourceOffset = 216;
/** Tag class of the container that groups components. */
inline constexpr std::uint32_t kContainerClass = 0x80808A54U;
/** A container holds its bubble mask at this offset. */
inline constexpr std::size_t kContainerBubbleMaskOffset = 8;
/** The mask is 32 bytes, one bit per map-global bubble index. */
inline constexpr std::size_t kContainerBubbleMaskBytes = 32;
/** Bubble indices the mask can name. No installed map declares one above 169. */
inline constexpr std::size_t kBubbleIndexCapacity = kContainerBubbleMaskBytes * 8;
/** A container holds its member array descriptor at this offset. */
inline constexpr std::size_t kContainerMemberDescriptor = 40;
/** Element class of a container's member array. */
inline constexpr std::uint32_t kContainerMemberClass = 0x80808BB0U;
/** One member is a bare tag handle. */
inline constexpr std::size_t kContainerMemberStride = 4;

/**
 * Reads the resource one component wraps.
 * @param blob Whole component bytes.
 * @param tag Receives the wrapped resource tag.
 * @return True when the field is inside the blob.
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
