#include "slot_descriptor_reader.h"

#include "definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/**
 * Tests the four predicates that together identify a descriptor.
 * @param blob Complete placed-object bytes.
 * @param base Candidate descriptor offset.
 * @param ownTag Tag handle of the blob.
 * @param registryKey Registry key of the owning object.
 * @param output Receives the descriptor fields when every predicate holds.
 * @return True when this offset is a descriptor.
 */
[[nodiscard]] bool descriptor_at(std::span<const std::byte> blob,
                                 std::size_t base,
                                 std::uint32_t ownTag,
                                 std::uint32_t registryKey,
                                 SlotDescriptor& output) noexcept {
    output = {};
    std::uint32_t tag = 0;
    std::uint32_t mark = 0;
    std::uint32_t key = 0;
    if (!read(blob, base + kDescriptorOwnTagOffset, tag) || tag != ownTag
        || !read(blob, base + kDescriptorMarkOffset, mark) || mark != kDescriptorMark
        || !read(blob, base + kDescriptorRegistryKeyOffset, key) || key != registryKey) {
        return false;
    }
    SlotDescriptor candidate{};
    if (!read(blob, base + kDescriptorComponentClassOffset, candidate.componentClass)
        || !read(blob, base + kDescriptorSenseSchemaOffset, candidate.senseSchema)
        || !read(blob, base + kDescriptorAuthSchemaOffset, candidate.authSchema)
        || !read(blob, base + kDescriptorSlotTypeOffset, candidate.slotType)
        || !read(blob, base + kDescriptorSlotIndexOffset, candidate.slotIndex)) {
        return false;
    }
    // A slot type that declares no auth or no sense schema records the absent sentinel there, so
    // requiring a class in both fields drops every such descriptor.
    if (!is_class_id(candidate.componentClass) || !is_schema_id(candidate.senseSchema)
        || !is_schema_id(candidate.authSchema)) {
        return false;
    }
    output = candidate;
    return true;
}

} // namespace

/**
 * Reports every slot descriptor inside one placed-object blob.
 * @param blob Complete placed-object bytes.
 * @param ownTag Tag handle of that blob.
 * @param registryKey Registry key of the object that reached it.
 * @param visitor Required non-owning descriptor consumer.
 * @param context Caller context passed to the visitor.
 * @return True when the whole blob is walked and the visitor accepts every descriptor.
 */
bool visit_slot_descriptors(std::span<const std::byte> blob,
                            std::uint32_t ownTag,
                            std::uint32_t registryKey,
                            DescriptorVisitor visitor,
                            void* context) noexcept {
    if (visitor == nullptr) {
        return false;
    }
    if (blob.size() < kDescriptorSize) {
        return true;
    }
    const std::size_t last = blob.size() - kDescriptorSize;
    for (std::size_t base = 0; base <= last; base += kDescriptorStep) {
        SlotDescriptor descriptor{};
        if (!descriptor_at(blob, base, ownTag, registryKey, descriptor)) {
            continue;
        }
        if (!visitor(context, descriptor)) {
            return false;
        }
    }
    return true;
}

/**
 * Reads the next tag on the chain from a placed object's handle to its descriptor blob.
 * @param blob Complete bytes of the blob just read.
 * @param classId Class the entry table records for it.
 * @param tag Receives the next tag.
 * @return True when this class is a chain hop and its next tag resolves.
 */
bool next_descriptor_tag(std::span<const std::byte> blob,
                         std::uint32_t classId,
                         std::uint32_t& tag) noexcept {
    tag = 0;
    if (classId == kSlotRedirectClass) {
        return read(blob, kSlotRedirectTagOffset, tag);
    }
    if (classId != kSlotIndirectClass) {
        return false;
    }
    Array handles{};
    return find_array_at(blob, kSlotIndirectDescriptor, handles) && handles.count != 0
           && read(blob, handles.dataOffset, tag);
}

} // namespace sunrise::middleware::content::packages::tables
