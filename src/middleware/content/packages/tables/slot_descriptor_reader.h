#pragma once

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
/** The indirect blob holds its handle array descriptor at this offset. */
inline constexpr std::size_t kSlotIndirectDescriptor = 16;
/** The redirect blob names its next tag at this offset. */
inline constexpr std::size_t kSlotRedirectTagOffset = 12;

/** Descriptor field offsets, all measured. */
inline constexpr std::size_t kDescriptorOwnTagOffset = 0;
inline constexpr std::size_t kDescriptorComponentClassOffset = 4;
inline constexpr std::size_t kDescriptorMarkOffset = 8;
inline constexpr std::size_t kDescriptorRegistryKeyOffset = 48;
inline constexpr std::size_t kDescriptorSlotTypeOffset = 52;
inline constexpr std::size_t kDescriptorSlotIndexOffset = 54;
inline constexpr std::size_t kDescriptorSenseSchemaOffset = 68;
inline constexpr std::size_t kDescriptorAuthSchemaOffset = 72;

/** What one slot type resolves to. */
struct SlotDescriptor {
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    std::uint16_t slotType{};
    std::uint16_t slotIndex{};
};

/** Visitor called once per descriptor. Returning false stops the walk and fails it. */
using DescriptorVisitor = bool (*)(void* context, const SlotDescriptor& descriptor) noexcept;

/** @param value Candidate field. @return True when it has the shape of a class id. */
[[nodiscard]] constexpr bool is_class_id(std::uint32_t value) noexcept {
    return (value >> 16U) == kClassHighHalf;
}

/** @param value Candidate schema field. @return True when it names a schema or declares none. */
[[nodiscard]] constexpr bool is_schema_id(std::uint32_t value) noexcept {
    return value == kAbsentSchema || is_class_id(value);
}

/**
 * Reports every slot descriptor inside one placed-object blob.
 * Four predicates must hold together: the blob's own tag repeated, the mark, the owning object's
 * registry key, and class ids throughout. Dropping the last admits 10 times the noise.
 *
 * @param blob Complete placed-object bytes.
 * @param ownTag Tag handle of that blob.
 * @param registryKey Registry key of the object that reached it.
 * @param visitor Required non-owning descriptor consumer.
 * @param context Caller context passed to the visitor.
 * @return True when the whole blob is walked and the visitor accepts every descriptor.
 */
[[nodiscard]] bool visit_slot_descriptors(std::span<const std::byte> blob,
                                          std::uint32_t ownTag,
                                          std::uint32_t registryKey,
                                          DescriptorVisitor visitor,
                                          void* context) noexcept;

/**
 * Reads the next tag on the chain from a placed object's handle to its descriptor blob.
 * @param blob Complete bytes of the blob just read.
 * @param classId Class the entry table records for it.
 * @param tag Receives the next tag.
 * @return True when this class is a chain hop and its next tag resolves.
 */
[[nodiscard]] bool next_descriptor_tag(std::span<const std::byte> blob,
                                       std::uint32_t classId,
                                       std::uint32_t& tag) noexcept;

} // namespace sunrise::middleware::content::packages::tables
