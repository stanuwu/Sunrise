#include "scenario_reader.h"

#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/** A state records whether it is enabled in its first byte. */
constexpr std::size_t kStateEnabledOffset = 0;
/** A state holds its own name hash at this offset. */
constexpr std::size_t kStateHashOffset = 4;
/** A state repeats its owning bubble's name hash at this offset. */
constexpr std::size_t kStateBubbleHashOffset = 64;
/** A state names its slice-set entry at this offset. */
constexpr std::size_t kStateEntryTagOffset = 68;
/**
 * A state carries the PUB/PRV public flag here.
 * The client getter returns the state array header plus 28, and data starts 16 bytes after that
 * header, so the byte is 12 into the record, where only 0 and 1 ever appear.
 */
constexpr std::size_t kStatePublicOffset = 12;

} // namespace

/**
 * Finds a scenario's bubble array.
 * @param blob Whole scenario bytes.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a bubble array.
 */
bool scenario_bubbles(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    return find_array_at(blob, kScenarioBubbleDescriptor, output);
}

/**
 * Reads one bubble and finds its slice-set state array.
 * @param blob Whole scenario bytes.
 * @param bubbles Bubble array that was found.
 * @param index Bubble ordinal.
 * @param output Receives the bubble.
 * @return True when the bubble is inside the blob.
 */
bool bubble_at(std::span<const std::byte> blob,
               const Array& bubbles,
               std::uint64_t index,
               Bubble& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (!element_offset(bubbles.dataOffset, bubbles.count, kBubbleStride, index, offset)
        || !read(blob, offset, output.nameHash)) {
        return false;
    }
    // A bubble with no states is ordinary, so a descriptor that does not read is not a failure.
    Array states{};
    if (find_array_at(blob, offset + kBubbleStateDescriptor, states)) {
        output.stateCount = states.count;
        output.stateDataOffset = states.dataOffset;
    }
    return true;
}

/**
 * Reads one slice-set state of a bubble.
 * @param blob Whole scenario bytes.
 * @param bubble Bubble carrying the state array.
 * @param index State ordinal.
 * @param output Receives the state.
 * @return True when the state is inside the blob.
 */
bool slice_state_at(std::span<const std::byte> blob,
                    const Bubble& bubble,
                    std::uint64_t index,
                    SliceState& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (!element_offset(
            bubble.stateDataOffset, bubble.stateCount, kSliceStateStride, index, offset)) {
        return false;
    }
    std::uint8_t enabled = 0;
    std::uint8_t isPublic = 0;
    if (!read(blob, offset + kStateEnabledOffset, enabled)
        || !read(blob, offset + kStateHashOffset, output.stateHash)
        || !read(blob, offset + kStatePublicOffset, isPublic)
        || !read(blob, offset + kStateMapBubbleIndexOffset, output.mapBubbleIndex)
        || !read(blob, offset + kStateBubbleHashOffset, output.bubbleNameHash)
        || !read(blob, offset + kStateEntryTagOffset, output.entryTag)) {
        output = {};
        return false;
    }
    output.enabled = enabled != 0;
    output.isPublic = isPublic != 0;
    return true;
}

/**
 * Reads the index and registry of one slice-set entry.
 * @param blob Whole slice-set entry bytes.
 * @param output Receives the entry fields.
 * @return True when both fields are inside the blob.
 */
bool slice_entry(std::span<const std::byte> blob, SliceEntry& output) noexcept {
    output = {};
    if (!read(blob, kSliceEntryIndexOffset, output.index)
        || !read(blob, kSliceEntryRegistryOffset, output.registryTag)) {
        output = {};
        return false;
    }
    return true;
}

/**
 * Finds one of a registry's three object tag arrays.
 * @param blob Whole registry bytes.
 * @param descriptorOffset One of the three registry descriptor offsets.
 * @param output Receives the array that was found.
 * @return True when that descriptor points at an array.
 */
bool registry_objects(std::span<const std::byte> blob,
                      std::size_t descriptorOffset,
                      Array& output) noexcept {
    output = {};
    return find_array_at(blob, descriptorOffset, output);
}

/**
 * Reads one object tag from a registry array.
 * @param blob Whole registry bytes.
 * @param objects Registry array that was found.
 * @param index Element ordinal.
 * @param tag Receives the object tag.
 * @return True when the ordinal is inside the array.
 */
bool registry_object_at(std::span<const std::byte> blob,
                        const Array& objects,
                        std::uint64_t index,
                        std::uint32_t& tag) noexcept {
    tag = 0;
    std::size_t offset = 0;
    return element_offset(objects.dataOffset, objects.count, kRegistryElementStride, index, offset)
           && read(blob, offset, tag);
}

/**
 * Reads a placed object's registry key.
 * @param blob Whole object bytes.
 * @param key Receives the key.
 * @return True when the key is inside the blob.
 */
bool object_key(std::span<const std::byte> blob, std::uint32_t& key) noexcept {
    key = 0;
    return read(blob, kObjectKeyOffset, key);
}

/**
 * Finds a placed object's slot array.
 * @param blob Whole object bytes.
 * @param output Receives the array that was found.
 * @return True when the object declares slots.
 */
bool object_slots(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    return find_array_at(blob, kObjectSlotDescriptor, output);
}

/**
 * Reads one slot of a placed object.
 * @param blob Whole object bytes.
 * @param slots Slot array that was found.
 * @param index Slot ordinal.
 * @param output Receives the slot.
 * @return True when the slot is inside the blob.
 */
bool object_slot_at(std::span<const std::byte> blob,
                    const Array& slots,
                    std::uint64_t index,
                    Slot& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (!element_offset(slots.dataOffset, slots.count, kSlotStride, index, offset)
        || !read(blob, offset, output.type)
        || !read(blob, offset + sizeof output.type, output.nameHash)) {
        output = {};
        return false;
    }
    return true;
}

/**
 * Finds a placed object's per-bubble sub-block array.
 * @param blob Whole object bytes.
 * @param output Receives the array that was found.
 * @return True when the object declares sub-blocks.
 */
bool object_bubbles(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    return find_array_at(blob, kObjectBubbleDescriptor, output);
}

/**
 * Reads one per-bubble sub-block and finds its placed handles.
 * @param blob Whole object bytes.
 * @param bubbles Sub-block array that was found.
 * @param index Sub-block ordinal.
 * @param output Receives the sub-block.
 * @return True when the sub-block is inside the blob.
 */
bool object_bubble_at(std::span<const std::byte> blob,
                      const Array& bubbles,
                      std::uint64_t index,
                      ObjectBubble& output) noexcept {
    output = {};
    std::size_t offset = 0;
    if (!element_offset(bubbles.dataOffset, bubbles.count, kObjectBubbleStride, index, offset)
        || !read(blob, offset, output.bubbleIndex)) {
        output = {};
        return false;
    }
    // A sub-block placing nothing is ordinary, so a descriptor that does not read is not a failure.
    Array handles{};
    if (find_array_at(blob, offset + kObjectBubbleHandleDescriptor, handles)) {
        output.handleCount = handles.count;
        output.handleDataOffset = handles.dataOffset;
    }
    return true;
}

/**
 * Reads one placed handle from a per-bubble sub-block.
 * @param blob Whole object bytes.
 * @param bubble Sub-block carrying the handle array.
 * @param index Handle ordinal.
 * @param handle Receives the handle.
 * @return True when the ordinal is inside the array.
 */
bool object_placed_handle_at(std::span<const std::byte> blob,
                             const ObjectBubble& bubble,
                             std::uint64_t index,
                             std::uint32_t& handle) noexcept {
    handle = 0;
    std::size_t offset = 0;
    return element_offset(bubble.handleDataOffset,
                          bubble.handleCount,
                          kObjectPlacedHandleStride,
                          index,
                          offset)
           && read(blob, offset, handle);
}

} // namespace sunrise::middleware::content::packages::tables
