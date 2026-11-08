#include "slot_descriptor_reader.h"

#include <array>
#include <limits>

#include "definition_index_table.h"
#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/** Tests the owner key and the class-shaped fields of the config's descriptor. */
[[nodiscard]] bool descriptor_at(std::span<const std::byte> blob,
                                 std::size_t base,
                                 std::uint32_t ownTag,
                                 std::uint32_t registryKey,
                                 SlotDescriptor& output) noexcept {
    output = {};
    std::uint32_t key = 0;
    if (!read(blob, base + kDescriptorRegistryKeyOffset, key) || key != registryKey) {
        return false;
    }
    SlotDescriptor candidate{};
    candidate.configTag = ownTag;
    candidate.descriptorOffset = static_cast<std::uint32_t>(base);
    if (!read(blob, base + kDescriptorComponentClassOffset, candidate.componentClass)
        || !read(blob, base + kDescriptorBubbleIndexOffset, candidate.bubbleIndex)
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

/** One tag waiting to be read by the bounded descriptor-chain walk. */
struct PendingTag {
    std::uint32_t tag{};
    std::size_t depth{};
};

/** One SDK chain tag plus the ancestors on only its branch. */
struct ObservedPendingTag final {
    std::uint32_t tag{};
    std::uint8_t depth{};
    std::uint8_t ancestorCount{};
    std::array<std::uint32_t, kPlacedChainDepthLimit> ancestors{};
};

/** One SDK-record pending tag plus exact authored child ordinals from its root. */
struct RecordPendingTag final {
    std::uint32_t tag{};
    std::uint8_t depth{};
    std::uint8_t ancestorCount{};
    std::array<std::uint32_t, kPlacedChainDepthLimit> ancestors{};
    std::array<std::uint8_t, kPlacedChainDepthLimit> branchPath{};
};

/** Adds one child to the bounded work stack. */
[[nodiscard]] bool push_child(std::array<PendingTag, kDescriptorChainCapacity>& pending,
                              std::size_t& pendingCount,
                              std::size_t& scheduledCount,
                              std::uint32_t tag,
                              std::size_t depth) noexcept {
    if (depth >= kDescriptorChainDepthLimit || pendingCount == pending.size()
        || scheduledCount == kDescriptorChainCapacity) {
        return false;
    }
    pending[pendingCount] = PendingTag{tag, depth};
    ++pendingCount;
    ++scheduledCount;
    return true;
}

/** Adds one observed child while retaining its path-local ancestors. */
[[nodiscard]] bool
push_observed_child(std::array<ObservedPendingTag, kPlacedChainCapacity>& pending,
                    std::size_t& pendingCount,
                    std::size_t& scheduledCount,
                    const ObservedPendingTag& parent,
                    std::uint32_t tag) noexcept {
    const std::size_t depth = static_cast<std::size_t>(parent.depth) + 1U;
    if (depth >= kPlacedChainDepthLimit || pendingCount == pending.size()
        || scheduledCount == kPlacedChainCapacity
        || parent.ancestorCount >= parent.ancestors.size()) {
        return false;
    }
    ObservedPendingTag child{};
    child.tag = tag;
    child.depth = static_cast<std::uint8_t>(depth);
    child.ancestorCount = static_cast<std::uint8_t>(parent.ancestorCount + 1U);
    for (std::size_t index = 0; index < parent.ancestorCount; ++index) {
        child.ancestors[index] = parent.ancestors[index];
    }
    child.ancestors[parent.ancestorCount] = parent.tag;
    pending[pendingCount] = child;
    ++pendingCount;
    ++scheduledCount;
    return true;
}

/** Adds one exact record child while retaining path-local tags and authored branch ordinals. */
[[nodiscard]] bool push_record_child(std::array<RecordPendingTag, kPlacedChainCapacity>& pending,
                                     std::size_t& pendingCount,
                                     std::size_t& scheduledCount,
                                     const RecordPendingTag& parent,
                                     std::uint32_t tag,
                                     std::uint64_t branchOrdinal) noexcept {
    const std::size_t depth = static_cast<std::size_t>(parent.depth) + 1U;
    if (depth >= kPlacedChainDepthLimit || pendingCount == pending.size()
        || scheduledCount == kPlacedChainCapacity || parent.ancestorCount >= parent.ancestors.size()
        || branchOrdinal > (std::numeric_limits<std::uint8_t>::max)()) {
        return false;
    }
    RecordPendingTag child{};
    child.tag = tag;
    child.depth = static_cast<std::uint8_t>(depth);
    child.ancestorCount = static_cast<std::uint8_t>(parent.ancestorCount + 1U);
    for (std::size_t index = 0; index < parent.ancestorCount; ++index) {
        child.ancestors[index] = parent.ancestors[index];
        child.branchPath[index] = parent.branchPath[index];
    }
    child.ancestors[parent.ancestorCount] = parent.tag;
    child.branchPath[parent.ancestorCount] = static_cast<std::uint8_t>(branchOrdinal);
    pending[pendingCount] = child;
    ++pendingCount;
    ++scheduledCount;
    return true;
}

/** Increments one retained u32 count without wrapping. */
[[nodiscard]] bool increment(std::uint32_t& value) noexcept {
    if (value == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    ++value;
    return true;
}

} // namespace

/** Finds the config's definition object and checks that its handle names the config. */
bool config_descriptor_offset(std::span<const std::byte> blob,
                              std::uint32_t ownTag,
                              std::size_t& offset) noexcept {
    offset = 0;
    std::size_t descriptor = 0;
    std::size_t defaultInstance = 0;
    std::uint32_t tag = 0;
    std::int64_t handleOffset = 0;
    if (!relative(blob, kConfigDescriptorField, descriptor)
        || !relative(blob, kConfigDefaultInstanceField, defaultInstance)
        || sizeof(SlotDescriptorHeader) > blob.size() - descriptor
        || !read(blob, descriptor + kDescriptorOwnTagOffset, tag) || tag != ownTag
        || !read(blob, descriptor + kDescriptorHandleOffsetOffset, handleOffset)
        || handleOffset != static_cast<std::int64_t>(defaultInstance)) {
        return false;
    }
    offset = descriptor;
    return true;
}

/** Reports the config's descriptor unless it is absent or names another owner. */
bool visit_slot_descriptors(std::span<const std::byte> blob,
                            std::uint32_t ownTag,
                            std::uint32_t registryKey,
                            DescriptorVisitor visitor,
                            void* context) noexcept {
    if (visitor == nullptr) {
        return false;
    }
    std::size_t base = 0;
    SlotDescriptor descriptor{};
    if (!config_descriptor_offset(blob, ownTag, base)
        || !descriptor_at(blob, base, ownTag, registryKey, descriptor)) {
        return true;
    }
    return visitor(context, descriptor);
}

/** Walks every known descriptor-chain branch with fixed work storage. */
bool walk_slot_descriptor_chain(std::uint32_t rootTag,
                                std::uint32_t registryKey,
                                DescriptorChainReader reader,
                                void* readerContext,
                                DescriptorVisitor visitor,
                                void* visitorContext) noexcept {
    if (reader == nullptr || visitor == nullptr) {
        return false;
    }
    std::array<PendingTag, kDescriptorChainCapacity> pending{};
    pending[0] = PendingTag{rootTag, 0};
    std::size_t pendingCount = 1;
    std::size_t scheduledCount = 1;
    while (pendingCount != 0) {
        const PendingTag current = pending[--pendingCount];
        std::span<const std::byte> blob;
        std::uint32_t classId = 0;
        if (!reader(readerContext, current.tag, blob, classId)) {
            return false;
        }
        if (classId == kPlacedObjectClass) {
            if (!visit_slot_descriptors(blob, current.tag, registryKey, visitor, visitorContext)) {
                return false;
            }
            continue;
        }
        if (classId == kSlotRedirectClass) {
            std::uint32_t next = 0;
            if (!read(blob, kSlotRedirectTagOffset, next)
                || !push_child(pending, pendingCount, scheduledCount, next, current.depth + 1)) {
                return false;
            }
            continue;
        }
        if (classId != kSlotIndirectClass) {
            return false;
        }
        std::uint32_t direct = 0;
        if (!read(blob, kSlotDirectTagOffset, direct)) {
            return false;
        }
        if (direct != kSlotDirectTagAbsent) {
            // This is the ordinary authored object-list branch, not a descriptor redirect.
            continue;
        }
        Array targets{};
        if (!find_array_at(blob, kSlotIndirectDescriptor, targets) || targets.count == 0
            || targets.elementClass != kSlotRedirectElementClass
            || targets.count > kDescriptorChainCapacity - scheduledCount
            || targets.dataOffset > blob.size()
            || targets.count > (blob.size() - targets.dataOffset) / kSlotRedirectElementStride
            || current.depth + 1 >= kDescriptorChainDepthLimit) {
            return false;
        }
        // Push in reverse so the authored first child is visited first by the LIFO stack.
        for (std::uint64_t ordinal = targets.count; ordinal != 0; --ordinal) {
            std::uint32_t next = 0;
            const std::size_t offset =
                targets.dataOffset
                + static_cast<std::size_t>(ordinal - 1) * kSlotRedirectElementStride;
            if (!read(blob, offset, next)
                || !push_child(pending, pendingCount, scheduledCount, next, current.depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

/** Observes every bounded branch and keeps safe counts after a partial branch. */
bool observe_placed_chain(std::uint32_t rootTag,
                          DescriptorChainReader reader,
                          void* readerContext,
                          PlacedConfigVisitor configVisitor,
                          void* configContext,
                          PlacedBareTargetVisitor bareTargetVisitor,
                          void* bareTargetContext,
                          PlacedChainObservation& output) noexcept {
    output = {};
    if (reader == nullptr || configVisitor == nullptr || bareTargetVisitor == nullptr) {
        return false;
    }
    output.complete = true;
    std::array<ObservedPendingTag, kPlacedChainCapacity> pending{};
    pending[0].tag = rootTag;
    std::size_t pendingCount = 1;
    std::size_t scheduledCount = 1;
    while (pendingCount != 0) {
        const ObservedPendingTag current = pending[--pendingCount];
        bool cycle = false;
        for (std::size_t index = 0; index < current.ancestorCount; ++index) {
            if (current.ancestors[index] == current.tag) {
                cycle = true;
                break;
            }
        }
        if (cycle) {
            output.complete = false;
            continue;
        }

        std::span<const std::byte> blob;
        std::uint32_t classId = 0;
        if (!reader(readerContext, current.tag, blob, classId)) {
            output.complete = false;
            continue;
        }
        if (!increment(output.hopCount)) {
            output.complete = false;
            continue;
        }
        if (classId == kPlacedObjectClass) {
            if (!configVisitor(configContext, current.tag, blob)) {
                output.complete = false;
            }
            continue;
        }
        if (classId == kSlotRedirectClass) {
            std::uint32_t next = 0;
            if (!read(blob, kSlotRedirectTagOffset, next)
                || !push_observed_child(pending, pendingCount, scheduledCount, current, next)) {
                output.complete = false;
            }
            continue;
        }
        if (classId != kSlotIndirectClass) {
            output.complete = false;
            continue;
        }

        std::uint32_t direct = 0;
        if (!read(blob, kSlotDirectTagOffset, direct)) {
            output.complete = false;
            continue;
        }
        if (direct != kSlotDirectTagAbsent) {
            if (!increment(output.bareTargetCount)
                || !bareTargetVisitor(bareTargetContext, current.tag, direct)) {
                output.complete = false;
            }
            continue;
        }

        Array targets{};
        if (!find_array_at(blob, kSlotIndirectDescriptor, targets) || targets.count == 0
            || targets.elementClass != kSlotRedirectElementClass || targets.dataOffset > blob.size()
            || targets.count > (blob.size() - targets.dataOffset) / kSlotRedirectElementStride
            || static_cast<std::size_t>(current.depth) + 1U >= kPlacedChainDepthLimit) {
            output.complete = false;
            continue;
        }
        const std::size_t available = kPlacedChainCapacity - scheduledCount;
        const std::uint64_t accepted = targets.count < available ? targets.count : available;
        if (accepted != targets.count) {
            output.complete = false;
        }
        for (std::uint64_t ordinal = accepted; ordinal != 0; --ordinal) {
            std::uint32_t next = 0;
            const std::size_t offset =
                targets.dataOffset
                + static_cast<std::size_t>(ordinal - 1U) * kSlotRedirectElementStride;
            if (!read(blob, offset, next)
                || !push_observed_child(pending, pendingCount, scheduledCount, current, next)) {
                output.complete = false;
            }
        }
    }
    return output.complete;
}

/** Visits every exact path-specific node without collapsing repeated tags. */
bool visit_placed_chain_records(std::uint32_t rootTag,
                                DescriptorChainReader reader,
                                void* readerContext,
                                PlacedChainRecordVisitor visitor,
                                void* visitorContext,
                                PlacedChainObservation& output) noexcept {
    output = {};
    if (reader == nullptr || visitor == nullptr) {
        return false;
    }
    output.complete = true;
    std::array<RecordPendingTag, kPlacedChainCapacity> pending{};
    pending[0].tag = rootTag;
    std::size_t pendingCount = 1;
    std::size_t scheduledCount = 1;
    while (pendingCount != 0) {
        const RecordPendingTag current = pending[--pendingCount];
        for (std::size_t index = 0; index < current.ancestorCount; ++index) {
            if (current.ancestors[index] == current.tag) {
                output.complete = false;
                return false;
            }
        }

        std::span<const std::byte> blob;
        std::uint32_t classId = 0;
        if (!reader(readerContext, current.tag, blob, classId) || !increment(output.hopCount)) {
            output.complete = false;
            return false;
        }
        PlacedChainRecord record{};
        record.tag = current.tag;
        record.classId = classId;
        record.depth = current.depth;
        record.branchPathCount = current.depth;
        record.branchPath = current.branchPath;
        if (classId == kPlacedObjectClass) {
            record.shape = PlacedChainShape::config;
            if (!visitor(visitorContext, record, blob)) {
                output.complete = false;
                return false;
            }
            continue;
        }
        if (classId == kSlotRedirectClass) {
            std::uint32_t next = 0;
            record.shape = PlacedChainShape::redirect;
            record.childCount = 1;
            if (!read(blob, kSlotRedirectTagOffset, next) || !visitor(visitorContext, record, blob)
                || !push_record_child(pending, pendingCount, scheduledCount, current, next, 0)) {
                output.complete = false;
                return false;
            }
            continue;
        }
        if (classId != kSlotIndirectClass) {
            output.complete = false;
            return false;
        }

        std::uint32_t direct = 0;
        if (!read(blob, kSlotDirectTagOffset, direct)) {
            output.complete = false;
            return false;
        }
        if (direct != kSlotDirectTagAbsent) {
            record.shape = PlacedChainShape::bareObjectList;
            record.directTargetTag = direct;
            if (!increment(output.bareTargetCount) || !visitor(visitorContext, record, blob)) {
                output.complete = false;
                return false;
            }
            continue;
        }

        Array targets{};
        if (!find_array_at(blob, kSlotIndirectDescriptor, targets) || targets.count == 0
            || targets.elementClass != kSlotRedirectElementClass || targets.dataOffset > blob.size()
            || targets.count > (blob.size() - targets.dataOffset) / kSlotRedirectElementStride
            || targets.count > kPlacedChainCapacity - scheduledCount
            || targets.count > (std::numeric_limits<std::uint8_t>::max)()
            || static_cast<std::size_t>(current.depth) + 1U >= kPlacedChainDepthLimit) {
            output.complete = false;
            return false;
        }
        record.shape = PlacedChainShape::descriptorRedirectArray;
        record.childCount = static_cast<std::uint32_t>(targets.count);
        if (!visitor(visitorContext, record, blob)) {
            output.complete = false;
            return false;
        }
        // Push in reverse so visits preserve the authored array order.
        for (std::uint64_t ordinal = targets.count; ordinal != 0; --ordinal) {
            std::uint32_t next = 0;
            const std::uint64_t childOrdinal = ordinal - 1U;
            const std::size_t offset =
                targets.dataOffset
                + static_cast<std::size_t>(childOrdinal) * kSlotRedirectElementStride;
            if (!read(blob, offset, next)
                || !push_record_child(
                    pending, pendingCount, scheduledCount, current, next, childOrdinal)) {
                output.complete = false;
                return false;
            }
        }
    }
    return true;
}

/** Reads the next tag only for the two known descriptor-chain classes. */
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
    std::uint32_t direct = 0;
    if (!read(blob, kSlotDirectTagOffset, direct) || direct != kSlotDirectTagAbsent) {
        return false;
    }
    Array handles{};
    return find_array_at(blob, kSlotIndirectDescriptor, handles) && handles.count != 0
           && handles.elementClass == kSlotRedirectElementClass
           && read(blob, handles.dataOffset, tag);
}

} // namespace sunrise::middleware::content::packages::tables
