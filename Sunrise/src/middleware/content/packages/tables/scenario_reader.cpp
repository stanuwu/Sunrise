#include "scenario_reader.h"

#include <cstddef>
#include <limits>
#include <type_traits>

#include "../reader/layout.h"
#include "../reader/typed_array.h"
#include "../schema/generated/tag_classes_808092.h"
#include "../schema/generated/tag_classes_808094.h"
#include "../schema/generated/tag_classes_808099.h"
#include "../schema/generated/tag_classes_80809b.h"
#include "../schema/generated/tag_classes_80809f.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

namespace generated = schema::generated;

/** A state records whether it is enabled in its first byte. */
constexpr std::size_t kStateEnabledOffset = offsetof(generated::SliceSetState, enabled);
/** A state holds its own name hash here. */
constexpr std::size_t kStateHashOffset = offsetof(generated::SliceSetState, stateHash);
/** A state repeats its owning bubble's name hash here. */
constexpr std::size_t kStateBubbleHashOffset = offsetof(generated::SliceSetState, bubbleNameHash);
/** A state names its slice-set entry here. */
constexpr std::size_t kStateEntryTagOffset = offsetof(generated::SliceSetState, entry);
/** Leading fields of a bubble row and a per-bubble sub-block. */
constexpr std::size_t kBubbleNameHashOffset = offsetof(generated::ScenarioBubble, nameHash);
constexpr std::size_t kObjectBubbleIndexOffset =
    offsetof(generated::ObjectBubbleBlock, bubbleIndex);
/** A slot is a type, then a name hash. */
constexpr std::size_t kSlotTypeOffset = offsetof(generated::ObjectSlot, slotType);
constexpr std::size_t kSlotNameHashOffset = offsetof(generated::ObjectSlot, nameHash);
/** A state carries the PUB/PRV public flag here; only 0 and 1 ever appear. */
constexpr std::size_t kStatePublicOffset = offsetof(generated::SliceSetState, publicFlag);

/** True when `Field` is an array field whose rows are `Row`. */
template <typename Field, typename Row>
concept ArrayOf = std::is_same_v<Field, generated::TagArray<Row>>;

// Every array field this reader walks must hold the rows its stride assumes.
static_assert(ArrayOf<decltype(generated::SScenario::bubbles), generated::ScenarioBubble>);
static_assert(ArrayOf<decltype(generated::ScenarioBubble::states), generated::SliceSetState>);
static_assert(ArrayOf<decltype(generated::SScenarioTagSet::tagSetA), generated::RegistryObjectRef>);
static_assert(ArrayOf<decltype(generated::C80809b38::slots), generated::ObjectSlot>);
static_assert(
    ArrayOf<decltype(generated::SAuthoredObject::entryFilters), generated::ObjectBubbleBlock>);
static_assert(ArrayOf<decltype(generated::ObjectBubbleBlock::handles), generated::PlacedHandle>);
/** Canonical SDK extraction refuses Tiger arrays above this authored-row bound. */
constexpr std::int64_t kObjectArrayMaximumCount = 1'000'000;

/** Reads an exact object array while accepting both authored empty forms. */
[[nodiscard]] bool object_array_at(std::span<const std::byte> blob,
                                   std::size_t descriptor,
                                   std::uint32_t expectedClass,
                                   std::size_t stride,
                                   Array& output) noexcept {
    output = {};
    std::int64_t signedCount = 0;
    std::int64_t relative = 0;
    if (!read(blob, descriptor, signedCount)
        || !read(blob, descriptor + reader::layout::kArrayFieldRelative, relative)
        || signedCount < 0 || signedCount > kObjectArrayMaximumCount) {
        return false;
    }
    if (signedCount == 0 && relative == 0) {
        return true;
    }
    if (descriptor > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())
                         - reader::layout::kArrayFieldRelative) {
        return false;
    }
    const std::int64_t base =
        static_cast<std::int64_t>(descriptor + reader::layout::kArrayFieldRelative);
    if ((relative > 0 && base > (std::numeric_limits<std::int64_t>::max)() - relative)
        || (relative < 0 && base < (std::numeric_limits<std::int64_t>::min)() - relative)) {
        return false;
    }
    const std::int64_t signedHeader = base + relative;
    if (signedHeader < 0) {
        return false;
    }
    const auto count = static_cast<std::uint64_t>(signedCount);
    const auto header = static_cast<std::size_t>(signedHeader);
    reader::TypedArrayRows rows{};
    std::uint32_t padding = 0;
    // An object array also needs zero header padding.
    if (!reader::read_typed_array(blob, header, count, expectedClass, stride, rows)
        || !read(blob, header + reader::layout::TypedArrayHeader::kPadding, padding)
        || padding != 0) {
        return false;
    }
    output = Array{count, rows.data, rows.elementClass};
    return true;
}

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
        || !read(blob, offset + kBubbleNameHashOffset, output.nameHash)) {
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
    if (!read(blob, offset + kStateEnabledOffset, enabled)
        || !read(blob, offset + kStateHashOffset, output.stateHash)
        || !read(blob, offset + kStatePublicOffset, output.rawU32At12)
        || !read(blob, offset + kStateMapBubbleIndexOffset, output.mapBubbleIndex)
        || !read(blob, offset + kStateBubbleHashOffset, output.bubbleNameHash)
        || !read(blob, offset + kStateEntryTagOffset, output.entryTag)) {
        output = {};
        return false;
    }
    output.enabled = enabled != 0;
    output.isPublic = (output.rawU32At12 & 0xFFU) != 0;
    return true;
}

/**
 * Reads the index and registry of one slice-set entry.
 * @param blob Whole slice-set entry bytes.
 * @param output Receives the entry fields.
 * @return True when both fields fit and the scaled index stays in range.
 */
bool slice_entry(std::span<const std::byte> blob, SliceEntry& output) noexcept {
    output = {};
    if (!read(blob, kSliceEntryIndexOffset, output.index)
        || !read(blob, kSliceEntryRegistryOffset, output.registryTag)
        || output.index > (std::numeric_limits<std::uint32_t>::max)() / kSliceSetIndexFactor) {
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
    return object_array_at(blob, kObjectSlotDescriptor, kObjectSlotClass, kSlotStride, output);
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
        || !read(blob, offset + kSlotTypeOffset, output.type)
        || !read(blob, offset + kSlotNameHashOffset, output.nameHash)) {
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
    return object_array_at(
        blob, kObjectBubbleDescriptor, kObjectBubbleClass, kObjectBubbleStride, output);
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
        || !read(blob, offset + kObjectBubbleIndexOffset, output.bubbleIndex)) {
        output = {};
        return false;
    }
    Array handles{};
    if (!object_array_at(blob,
                         offset + kObjectBubbleHandleDescriptor,
                         kObjectPlacedHandleClass,
                         kObjectPlacedHandleStride,
                         handles)) {
        output = {};
        return false;
    }
    output.handleCount = handles.count;
    output.handleDataOffset = handles.dataOffset;
    output.sourceOffset = offset;
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
