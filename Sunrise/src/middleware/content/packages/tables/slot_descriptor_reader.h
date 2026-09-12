#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::content::packages::tables {

/** Tag class of a placed-object blob, which is where slot descriptors live. */
inline constexpr std::uint32_t kPlacedObjectClass = 0x80809C36U;
/** A descriptor repeats this constant at its own offset 8. */
inline constexpr std::uint32_t kDescriptorMark = 0x70U;
/** One descriptor is this many bytes. */
inline constexpr std::size_t kDescriptorSize = 128;
/** Descriptors are found on this alignment inside the blob. */
inline constexpr std::size_t kDescriptorStep = 4;
/** A component class and a schema both carry this high half word. */
inline constexpr std::uint32_t kClassHighHalf = 0x8080U;
/** A slot type that declares no auth or no sense schema records this instead. */
inline constexpr std::uint32_t kAbsentSchema = 0xFFFFFFFFU;
/** Classes on the chain from a placed object's handle to its descriptor blob. */
inline constexpr std::uint32_t kSlotIndirectClass = 0x80809468U;
inline constexpr std::uint32_t kSlotRedirectClass = 0x80809B14U;
/** Element class of the indirect blob's descriptor redirect array. */
inline constexpr std::uint32_t kSlotRedirectElementClass = 0x80809B13U;
/** The indirect blob's direct object-list target, absent on the redirect-array form. */
inline constexpr std::size_t kSlotDirectTagOffset = 8;
inline constexpr std::uint32_t kSlotDirectTagAbsent = 0xFFFFFFFFU;
/** The indirect blob holds its handle array descriptor at this offset. */
inline constexpr std::size_t kSlotIndirectDescriptor = 16;
/** The redirect blob names its next tag at this offset. */
inline constexpr std::size_t kSlotRedirectTagOffset = 12;
/** Maximum nodes and depth one descriptor-chain walk may schedule. */
inline constexpr std::size_t kDescriptorChainCapacity = 256;
inline constexpr std::size_t kDescriptorChainDepthLimit = 8;
/** The SDK placed-chain walk matches the authored depth rule and bounds total scheduled work. */
inline constexpr std::size_t kPlacedChainCapacity = 256;
inline constexpr std::size_t kPlacedChainDepthLimit = 9;

/** Descriptor field offsets. */
inline constexpr std::size_t kDescriptorOwnTagOffset = 0;
inline constexpr std::size_t kDescriptorComponentClassOffset = 4;
inline constexpr std::size_t kDescriptorMarkOffset = 8;
inline constexpr std::size_t kDescriptorRegistryKeyOffset = 48;
inline constexpr std::size_t kDescriptorSlotTypeOffset = 52;
inline constexpr std::size_t kDescriptorSlotIndexOffset = 54;
inline constexpr std::size_t kDescriptorBubbleIndexOffset = 56;
inline constexpr std::size_t kDescriptorSenseSchemaOffset = 68;
inline constexpr std::size_t kDescriptorAuthSchemaOffset = 72;
/** Type 2 descriptors bind their authored squad through this ClientRef. */
inline constexpr std::size_t kType2SquadReferenceOffset = 104;

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
 * Reports descriptors matching the repeated tag, mark, owner key, and class-shaped fields.
 * The visitor may stop the bounded scan by returning false.
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
