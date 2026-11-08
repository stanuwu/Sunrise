#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../schema/generated/tag_classes_80807d.h"
#include "../schema/generated/tag_classes_808094.h"
#include "../schema/generated/tag_classes_808095.h"
#include "../schema/generated/tag_classes_80809a.h"
#include "../schema/generated/tag_classes_80809b.h"
#include "../schema/generated/tag_classes_80809c.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of a placed-object blob, which is where slot descriptors live. */
inline constexpr std::uint32_t kPlacedObjectClass = schema::generated::SEntityConfig::kClass;
/** A config names its slot descriptor and the default instance the descriptor's handle names. */
inline constexpr std::size_t kConfigDescriptorField =
    offsetof(schema::generated::SEntityConfig, definition);
inline constexpr std::size_t kConfigDefaultInstanceField =
    offsetof(schema::generated::SEntityConfig, defaultInstance);
/** A component class and a schema both carry this high half word. */
inline constexpr std::uint32_t kClassHighHalf = 0x8080U;
/** A slot type that declares no auth or no sense schema records this instead. */
inline constexpr std::uint32_t kAbsentSchema = 0xFFFFFFFFU;
/** Classes on the chain from a placed object's handle to its descriptor blob. */
inline constexpr std::uint32_t kSlotIndirectClass = schema::generated::SAuthoredObjectSet::kClass;
inline constexpr std::uint32_t kSlotRedirectClass = schema::generated::SEntityLink::kClass;
/** Element class and stride of the indirect blob's descriptor redirect array. */
inline constexpr std::uint32_t kSlotRedirectElementClass =
    schema::generated::SAuthoredObjectSetTarget::kClass;
inline constexpr std::size_t kSlotRedirectElementStride =
    sizeof(schema::generated::SAuthoredObjectSetTarget);
/** The indirect blob's direct object-list target, absent on the redirect-array form. */
inline constexpr std::size_t kSlotDirectTagOffset =
    offsetof(schema::generated::SAuthoredObjectSet, directTarget);
inline constexpr std::uint32_t kSlotDirectTagAbsent = 0xFFFFFFFFU;
/** The indirect blob holds its handle array descriptor at this offset. */
inline constexpr std::size_t kSlotIndirectDescriptor =
    offsetof(schema::generated::SAuthoredObjectSet, objects);
/** The redirect blob names its next tag at this offset. */
inline constexpr std::size_t kSlotRedirectTagOffset =
    offsetof(schema::generated::SEntityLink, next);
/** Maximum nodes and depth one descriptor-chain walk may schedule. */
inline constexpr std::size_t kDescriptorChainCapacity = 256;
inline constexpr std::size_t kDescriptorChainDepthLimit = 8;
/** The SDK placed-chain walk matches the authored depth rule and bounds total scheduled work. */
inline constexpr std::size_t kPlacedChainCapacity = 256;
inline constexpr std::size_t kPlacedChainDepthLimit = 9;

/** Descriptor field offsets; a descriptor is the shared slot component header. */
using SlotDescriptorHeader = schema::generated::SlotComponentHeader;
// Tag layout offsets inside the slot component header.
inline constexpr std::size_t kDescriptorOwnTagOffset = offsetof(SlotDescriptorHeader, handle.tag);
inline constexpr std::size_t kDescriptorComponentClassOffset =
    offsetof(SlotDescriptorHeader, handle.word);
inline constexpr std::size_t kDescriptorHandleOffsetOffset =
    offsetof(SlotDescriptorHeader, handle.offset);
inline constexpr std::size_t kDescriptorRegistryKeyOffset =
    offsetof(SlotDescriptorHeader, registryKey);
inline constexpr std::size_t kDescriptorSlotTypeOffset = offsetof(SlotDescriptorHeader, slotType);
inline constexpr std::size_t kDescriptorSlotIndexOffset = offsetof(SlotDescriptorHeader, slotIndex);
inline constexpr std::size_t kDescriptorBubbleIndexOffset =
    offsetof(SlotDescriptorHeader, bubbleIndex);
inline constexpr std::size_t kDescriptorSenseSchemaOffset =
    offsetof(SlotDescriptorHeader, senseSchema);
inline constexpr std::size_t kDescriptorAuthSchemaOffset =
    offsetof(SlotDescriptorHeader, authSchema);
/** Type 2 descriptors bind their authored squad through this ClientRef. */
inline constexpr std::size_t kType2SquadReferenceOffset =
    offsetof(schema::generated::Type2SquadComponent, squadTargetKey);
/** Type 30 descriptors name the volume or object their player monitor measures here. */
inline constexpr std::size_t kType30MeasuredReferenceOffset =
    offsetof(schema::generated::Type30MonitorComponent, measuredTargetKey);

/** What one slot type resolves to. */
struct SlotDescriptor {
    std::uint32_t configTag{};
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    std::uint32_t descriptorOffset{};
    std::uint32_t bubbleIndex{};
    std::uint16_t slotType{};
    std::uint16_t slotIndex{};
};

/** Visitor called once per descriptor. Returning false stops the walk and fails it. */
using DescriptorVisitor = bool (*)(void* context, const SlotDescriptor& descriptor) noexcept;

/** Injected package reader for one descriptor-chain tag. */
using DescriptorChainReader = bool (*)(void* context,
                                       std::uint32_t tag,
                                       std::span<const std::byte>& blob,
                                       std::uint32_t& classId) noexcept;

/** Counts safely observed rows even when one placed-chain branch is partial. */
struct PlacedChainObservation final {
    std::uint32_t hopCount{};
    std::uint32_t bareTargetCount{};
    bool complete{};
};

/** Exact terminal or branching shape of one validated node in a placed-object chain. */
enum class PlacedChainShape : std::uint8_t {
    config,
    redirect,
    descriptorRedirectArray,
    bareObjectList,
};

/**
 * One path-specific placed-chain node. `branchPath` stores authored child ordinals from the root;
 * two visits to the same package tag remain distinct when their paths differ.
 */
struct PlacedChainRecord final {
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::uint32_t directTargetTag{};
    std::uint32_t childCount{};
    std::uint8_t depth{};
    std::uint8_t branchPathCount{};
    std::array<std::uint8_t, kPlacedChainDepthLimit> branchPath{};
    PlacedChainShape shape{PlacedChainShape::config};
};

/** Visitor called for every path-specific config terminal. */
using PlacedConfigVisitor = bool (*)(void* context,
                                     std::uint32_t configTag,
                                     std::span<const std::byte> blob) noexcept;

/** Visitor called for every path-specific direct authored-placement target. */
using PlacedBareTargetVisitor = bool (*)(void* context,
                                         std::uint32_t sourceTag,
                                         std::uint32_t targetTag) noexcept;

/** Visitor for every fully validated path-specific node and its exact package payload. */
using PlacedChainRecordVisitor = bool (*)(void* context,
                                          const PlacedChainRecord& record,
                                          std::span<const std::byte> blob) noexcept;

/** @param value Candidate field. @return True when it has the shape of a class id. */
[[nodiscard]] constexpr bool is_class_id(std::uint32_t value) noexcept {
    return (value >> 16U) == kClassHighHalf;
}

/** @param value Candidate schema field. @return True when it names a schema or declares none. */
[[nodiscard]] constexpr bool is_schema_id(std::uint32_t value) noexcept {
    return value == kAbsentSchema || is_class_id(value);
}

/**
 * Finds the slot descriptor of one config: its definition object, whose handle names the config
 * and the config's default instance.
 * @param blob Whole config bytes.
 * @param ownTag The config's own tag.
 * @param offset Receives the descriptor offset.
 * @return False when the config carries no slot descriptor.
 */
[[nodiscard]] bool config_descriptor_offset(std::span<const std::byte> blob,
                                            std::uint32_t ownTag,
                                            std::size_t& offset) noexcept;

/**
 * Reports the config's descriptor when it names the owner key and has class-shaped fields.
 * The visitor may stop the walk by returning false.
 */
[[nodiscard]] bool visit_slot_descriptors(std::span<const std::byte> blob,
                                          std::uint32_t ownTag,
                                          std::uint32_t registryKey,
                                          DescriptorVisitor visitor,
                                          void* context) noexcept;

/**
 * Walks every branch from one placed handle to its descriptor blobs.
 * A bare indirect handle names an authored object list and ends successfully without a visit.
 * @param rootTag First placed handle tag.
 * @param registryKey Registry key every retained descriptor must name.
 * @param reader Injected package reader.
 * @param readerContext Opaque pointer handed to the reader.
 * @param visitor Descriptor consumer.
 * @param visitorContext Opaque pointer handed to the visitor.
 * @return True only when the bounded walk read and parsed every branch.
 */
[[nodiscard]] bool walk_slot_descriptor_chain(std::uint32_t rootTag,
                                              std::uint32_t registryKey,
                                              DescriptorChainReader reader,
                                              void* readerContext,
                                              DescriptorVisitor visitor,
                                              void* visitorContext) noexcept;

/**
 * Observes every bounded branch from one placed handle.
 * Depth and cycle failures happen before the rejected tag is read.
 * @return True only when every branch and visitor completed.
 */
[[nodiscard]] bool observe_placed_chain(std::uint32_t rootTag,
                                        DescriptorChainReader reader,
                                        void* readerContext,
                                        PlacedConfigVisitor configVisitor,
                                        void* configContext,
                                        PlacedBareTargetVisitor bareTargetVisitor,
                                        void* bareTargetContext,
                                        PlacedChainObservation& output) noexcept;

/**
 * Visits every node only after its exact class-specific shape validates. Branch order and repeated
 * path occurrences are retained; cycles, depth/capacity loss, malformed arrays, and visitor
 * refusal fail the complete walk.
 */
[[nodiscard]] bool visit_placed_chain_records(std::uint32_t rootTag,
                                              DescriptorChainReader reader,
                                              void* readerContext,
                                              PlacedChainRecordVisitor visitor,
                                              void* visitorContext,
                                              PlacedChainObservation& output) noexcept;

/** Reads the next tag only for a known descriptor-chain class. */
[[nodiscard]] bool next_descriptor_tag(std::span<const std::byte> blob,
                                       std::uint32_t classId,
                                       std::uint32_t& tag) noexcept;

} // namespace sunrise::middleware::content::packages::tables
