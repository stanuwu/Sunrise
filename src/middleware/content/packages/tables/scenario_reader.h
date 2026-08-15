#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of a destination's client scenario. */
inline constexpr std::uint32_t kScenarioClass = 0x80809994U;
/** A scenario holds its bubble array descriptor at this offset. */
inline constexpr std::size_t kScenarioBubbleDescriptor = 80;
/** Element class of a scenario's bubble array. */
inline constexpr std::uint32_t kBubbleClass = 0x8080924DU;
/** One bubble is 24 inline bytes. */
inline constexpr std::size_t kBubbleStride = 24;
/** A bubble holds its slice-set state array descriptor at this offset. */
inline constexpr std::size_t kBubbleStateDescriptor = 8;
/** Element class of a bubble's slice-set state array. */
inline constexpr std::uint32_t kSliceStateClass = 0x8080924FU;
/** One slice-set state is 76 inline bytes. */
inline constexpr std::size_t kSliceStateStride = 76;
/** Tag class of the slice-set entry a state names. */
inline constexpr std::uint32_t kSliceEntryClass = 0x8080925BU;
/** A slice-set entry holds its index at this offset. */
inline constexpr std::size_t kSliceEntryIndexOffset = 16;
/** A slice-set entry names its object registry at this offset. */
inline constexpr std::size_t kSliceEntryRegistryOffset = 20;
/** Slice-set indices are spaced by this factor across the region space. */
inline constexpr std::uint32_t kSliceSetIndexFactor = 8;
/**
 * A slice-set state holds its bubble's map-global index at this offset.
 * The index is the bit a container's bubble mask sets, so it binds a spawn set to a bubble.
 * It is not the scenario ordinal: one map's bubbles are shared by every activity on it.
 */
inline constexpr std::size_t kStateMapBubbleIndexOffset = 28;
/** Reported for a bubble whose index is absent or wider than a container mask can name. */
inline constexpr std::uint16_t kAbsentMapBubbleIndex = 0xFFFFU;
/** Tag class of the object registry a slice-set entry names. */
inline constexpr std::uint32_t kObjectRegistryClass = 0x8080925EU;
/** A registry holds three independent object tag arrays at these descriptor offsets. */
inline constexpr std::size_t kRegistryFirstDescriptor = 8;
inline constexpr std::size_t kRegistrySecondDescriptor = 24;
inline constexpr std::size_t kRegistryThirdDescriptor = 40;
/** Element class of a registry's object tag arrays. */
inline constexpr std::uint32_t kRegistryElementClass = 0x80809260U;
/** One registry element is a bare tag handle. */
inline constexpr std::size_t kRegistryElementStride = 4;
/** Tag class of a placed object. */
inline constexpr std::uint32_t kObjectClass = 0x80809462U;
/** An object holds its registry key at this offset. */
inline constexpr std::size_t kObjectKeyOffset = 12;
/** An object holds its slot array descriptor at this offset. */
inline constexpr std::size_t kObjectSlotDescriptor = 32;
/** One slot is a type and a name hash. */
inline constexpr std::size_t kSlotStride = 8;
/** An object holds its per-bubble sub-block array descriptor at this offset. */
inline constexpr std::size_t kObjectBubbleDescriptor = 56;
/** One per-bubble sub-block is 24 bytes. */
inline constexpr std::size_t kObjectBubbleStride = 24;
/** A sub-block holds its placed-handle array descriptor at this offset. */
inline constexpr std::size_t kObjectBubbleHandleDescriptor = 8;
/** One placed handle is a bare tag handle. */
inline constexpr std::size_t kObjectPlacedHandleStride = 4;
/** A sub-block that applies to every bubble records this index. */
inline constexpr std::int32_t kGlobalBubbleIndex = -1;

/** One slot of a placed object. */
struct Slot {
    std::uint32_t type{};
    std::uint32_t nameHash{};
};

/** One per-bubble sub-block of a placed object. */
struct ObjectBubble {
    std::int32_t bubbleIndex{};
    std::uint64_t handleCount{};
    std::size_t handleDataOffset{};
};

/** One bubble of a destination scenario. */
struct Bubble {
    std::uint32_t nameHash{};
    std::uint64_t stateCount{};
    std::size_t stateDataOffset{};
};

/** One slice-set state of a bubble. */
struct SliceState {
    std::uint32_t stateHash{};
    std::uint32_t bubbleNameHash{};
    std::uint32_t entryTag{};
    /** The bubble's map-global index, which container bubble masks are keyed by. */
    std::uint32_t mapBubbleIndex{};
    bool enabled{};
    /** A bubble is public when its first state carries this. The client prints PUB, else PRV. */
    bool isPublic{};
};

/** One slice-set entry named by a state. */
struct SliceEntry {
    std::uint32_t index{};
    std::uint32_t registryTag{};
};

/**
 * Finds a scenario's bubble array.
 * @param blob Whole scenario bytes.
 * @param output Receives the array that was found.
 * @return True when the descriptor points at a bubble array.
 */
[[nodiscard]] bool scenario_bubbles(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads one bubble and finds its slice-set state array.
 * A bubble with no readable state array reports a zero state count.
 * @param blob Whole scenario bytes.
 * @param bubbles Bubble array that was found.
 * @param index Bubble ordinal.
 * @param output Receives the bubble.
 * @return True when the bubble is inside the blob.
 */
[[nodiscard]] bool bubble_at(std::span<const std::byte> blob,
                             const Array& bubbles,
                             std::uint64_t index,
                             Bubble& output) noexcept;

/**
 * Reads one slice-set state of a bubble.
 * @param blob Whole scenario bytes.
 * @param bubble Bubble carrying the state array.
 * @param index State ordinal.
 * @param output Receives the state.
 * @return True when the state is inside the blob.
 */
[[nodiscard]] bool slice_state_at(std::span<const std::byte> blob,
                                  const Bubble& bubble,
                                  std::uint64_t index,
                                  SliceState& output) noexcept;

/**
 * Reads the index and registry of one slice-set entry.
 * @param blob Whole slice-set entry bytes.
 * @param output Receives the entry fields.
 * @return True when both fields are inside the blob.
 */
[[nodiscard]] bool slice_entry(std::span<const std::byte> blob, SliceEntry& output) noexcept;

/**
 * Finds one of a registry's three object tag arrays.
 * An array a registry does not declare reads as nothing, which is not an error.
 * @param blob Whole registry bytes.
 * @param descriptorOffset One of the three registry descriptor offsets.
 * @param output Receives the array that was found.
 * @return True when that descriptor points at an array.
 */
[[nodiscard]] bool registry_objects(std::span<const std::byte> blob,
                                    std::size_t descriptorOffset,
                                    Array& output) noexcept;

/**
 * Reads one object tag from a registry array.
 * @param blob Whole registry bytes.
 * @param objects Registry array that was found.
 * @param index Element ordinal.
 * @param tag Receives the object tag.
 * @return True when the ordinal is inside the array.
 */
[[nodiscard]] bool registry_object_at(std::span<const std::byte> blob,
                                      const Array& objects,
                                      std::uint64_t index,
                                      std::uint32_t& tag) noexcept;

/**
 * Reads a placed object's registry key.
 * @param blob Whole object bytes.
 * @param key Receives the key.
 * @return True when the key is inside the blob.
 */
[[nodiscard]] bool object_key(std::span<const std::byte> blob, std::uint32_t& key) noexcept;

/**
 * Finds a placed object's slot array.
 * @param blob Whole object bytes.
 * @param output Receives the array that was found.
 * @return True when the object declares slots.
 */
[[nodiscard]] bool object_slots(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads one slot of a placed object.
 * @param blob Whole object bytes.
 * @param slots Slot array that was found.
 * @param index Slot ordinal.
 * @param output Receives the slot.
 * @return True when the slot is inside the blob.
 */
[[nodiscard]] bool object_slot_at(std::span<const std::byte> blob,
                                  const Array& slots,
                                  std::uint64_t index,
                                  Slot& output) noexcept;

/**
 * Finds a placed object's per-bubble sub-block array.
 * @param blob Whole object bytes.
 * @param output Receives the array that was found.
 * @return True when the object declares sub-blocks.
 */
[[nodiscard]] bool object_bubbles(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads one per-bubble sub-block and finds its placed handles.
 * @param blob Whole object bytes.
 * @param bubbles Sub-block array that was found.
 * @param index Sub-block ordinal.
 * @param output Receives the sub-block.
 * @return True when the sub-block is inside the blob.
 */
[[nodiscard]] bool object_bubble_at(std::span<const std::byte> blob,
                                    const Array& bubbles,
                                    std::uint64_t index,
                                    ObjectBubble& output) noexcept;

/**
 * Reads one placed handle from a per-bubble sub-block.
 * @param blob Whole object bytes.
 * @param bubble Sub-block carrying the handle array.
 * @param index Handle ordinal.
 * @param handle Receives the handle.
 * @return True when the ordinal is inside the array.
 */
[[nodiscard]] bool object_placed_handle_at(std::span<const std::byte> blob,
                                           const ObjectBubble& bubble,
                                           std::uint64_t index,
                                           std::uint32_t& handle) noexcept;

} // namespace sunrise::middleware::content::packages::tables
