#include "component_container_reader.h"

#include <algorithm>

#include "../reader/container_framing.h"
#include "authored_placement_reader.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {

/** Reads the spawn set one component wraps. */
bool component_resource(std::span<const std::byte> blob, std::uint32_t& tag) noexcept {
    tag = 0;
    Array placements{};
    std::size_t payload = 0;
    std::uint32_t payloadClass = 0;
    return find_array_at(blob, kAuthoredPlacementDescriptor, placements)
           && placements.elementClass == kAuthoredPlacementClass
           && relative(blob, placements.dataOffset + kAuthoredPlacementAuxiliaryOffset, payload)
           && reader::layout::class_word_before(blob, payload, payloadClass)
           && payloadClass == kComponentSpawnPayloadClass
           && read(blob, payload + kComponentSpawnSetOffset, tag);
}

/** Reads the bubble mask of one container. */
bool container_bubble_mask(std::span<const std::byte> blob,
                           std::span<std::uint8_t> output) noexcept {
    if (output.size() != kContainerBubbleMaskBytes) {
        return false;
    }
    std::fill(output.begin(), output.end(), std::uint8_t{});
    for (std::size_t index = 0; index < kContainerBubbleMaskBytes; ++index) {
        if (!read(blob, kContainerBubbleMaskOffset + index, output[index])) {
            std::fill(output.begin(), output.end(), std::uint8_t{});
            return false;
        }
    }
    return true;
}

/** Finds the member array of one container. */
bool container_members(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    return find_array_at(blob, kContainerMemberDescriptor, output);
}

/** Reads one member tag of a container. */
bool container_member_at(std::span<const std::byte> blob,
                         const Array& members,
                         std::uint64_t index,
                         std::uint32_t& tag) noexcept {
    tag = 0;
    std::size_t offset = 0;
    return element_offset(members.dataOffset, members.count, kContainerMemberStride, index, offset)
           && read(blob, offset, tag);
}

/** Tests one bubble index against a mask. */
bool bubble_in_mask(std::span<const std::uint8_t> mask, std::size_t index) noexcept {
    const std::size_t byte = index / 8;
    return byte < mask.size() && (mask[byte] >> (index % 8) & 1U) != 0;
}

} // namespace sunrise::middleware::content::packages::tables
