#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/type23_placement_identifier_reader.h"
#include "../../../middleware/crypto/sha256.h"
#include "../../../state/build_data/scenarios/definition.h"
#include "scriptable_catalog_worker_internal.h"

namespace sunrise::client::content::activity::scriptables::worker_internal {
namespace {

namespace sha256 = middleware::crypto::sha256;

struct DescriptorContext final {
    Analysis* analysis{};
    const std::vector<AnalysisSlot>* slots{};
    std::span<const std::byte> config{};
};

/** Per-chain state carried through one placed-object walk. */
struct PlacedChainContext final {
    BuildContext* build{};
    Analysis* analysis{};
    std::uint32_t registryKey{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t subblockRow{};
    std::uint32_t leafRow{};
    std::uint32_t subblockOrdinal{};
    std::uint32_t leafOrdinal{};
    bool leafComplete{true};
    bool fatal{};
};

/** Retains descriptors that match the owning object's exact declared slot. */
[[nodiscard]] bool collect_descriptor(void* opaque,
                                      const tables::SlotDescriptor& descriptor) noexcept {
    auto& context = *static_cast<DescriptorContext*>(opaque);
    if (context.analysis == nullptr || context.slots == nullptr
        || descriptor.slotIndex >= context.slots->size()
        || (*context.slots)[descriptor.slotIndex].type != descriptor.slotType) {
        return true;
    }
    try {
        AnalysisDescriptor row{};
        row.descriptor = descriptor;
        row.placementIdentifierRead = tables::type23_placement_identifier(
            context.config, descriptor, row.placementIdentifier);
        context.analysis->descriptors.push_back(row);
        internal::RawReference reference{};
        if (internal::read_type2_squad_reference(context.config, descriptor, reference)) {
            context.analysis->references.push_back(reference);
        }
    } catch (...) {
        return false;
    }
    return true;
}

/** Supplies one tag to the path-aware placed-chain observer. */
[[nodiscard]] bool placed_chain_reader(void* opaque,
                                       std::uint32_t tag,
                                       std::span<const std::byte>& blob,
                                       std::uint32_t& classId) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PlacedChainContext*>(opaque);
    if (context.build == nullptr || !read_tag(*context.build, tag, context.build->chain, classId)) {
        blob = {};
        return false;
    }
    blob = context.build->chain;
    return true;
}

/** Retains one unique config and rolls back its rows when parsing fails. */
[[nodiscard]] bool
collect_placed_config(void* opaque, std::uint32_t tag, std::span<const std::byte> blob) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PlacedChainContext*>(opaque);
    if (context.analysis == nullptr) {
        return false;
    }
    Analysis& analysis = *context.analysis;
    try {
        if (std::find(analysis.observedConfigs.begin(), analysis.observedConfigs.end(), tag)
            == analysis.observedConfigs.end()) {
            analysis.observedConfigs.push_back(tag);
        }
    } catch (...) {
        return false;
    }
    if (std::find(analysis.resolvedConfigs.begin(), analysis.resolvedConfigs.end(), tag)
        != analysis.resolvedConfigs.end()) {
        return true;
    }
    const std::size_t firstDescriptor = analysis.descriptors.size();
    const std::size_t firstReference = analysis.references.size();
    DescriptorContext descriptorContext{&analysis, &analysis.slots, blob};
    if (!tables::visit_slot_descriptors(
            blob, tag, context.registryKey, &collect_descriptor, &descriptorContext)) {
        analysis.descriptors.resize(firstDescriptor);
        analysis.references.resize(firstReference);
        return false;
    }
    try {
        internal::collect_typed_references(blob, tag, analysis.references);
        analysis.resolvedConfigs.push_back(tag);
        return true;
    } catch (...) {
        analysis.descriptors.resize(firstDescriptor);
        analysis.references.resize(firstReference);
        return false;
    }
}

/** Converts one retained vector index to the SDK's exact u32 row domain. */
[[nodiscard]] bool row_index(std::size_t value, std::uint32_t& output) noexcept {
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
        output = catalog::kNoRow;
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/** Maps the validated package-reader shape without assigning new semantics. */
[[nodiscard]] constexpr catalog::PlacedHopShape
placed_hop_shape(tables::PlacedChainShape value) noexcept {
    switch (value) {
    case tables::PlacedChainShape::config:
        return catalog::PlacedHopShape::config;
    case tables::PlacedChainShape::redirect:
        return catalog::PlacedHopShape::redirect;
    case tables::PlacedChainShape::descriptorRedirectArray:
        return catalog::PlacedHopShape::descriptorRedirectArray;
    case tables::PlacedChainShape::bareObjectList:
        return catalog::PlacedHopShape::bareObjectList;
    }
    return catalog::PlacedHopShape::config;
}

/** Retains one exact path-specific hop and any terminal config or object-list edge. */
[[nodiscard]] bool collect_placed_chain_record(void* opaque,
                                               const tables::PlacedChainRecord& source,
                                               std::span<const std::byte> blob) noexcept {
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PlacedChainContext*>(opaque);
    if (context.build == nullptr || context.analysis == nullptr
        || context.subblockRow >= context.analysis->placedSubblocks.size()
        || context.leafRow >= context.analysis->placedLeaves.size()
        || source.branchPathCount > catalog::kPlacedBranchPathCapacity) {
        context.fatal = true;
        return false;
    }
    Analysis& analysis = *context.analysis;
    std::uint32_t hopRow = 0;
    if (!row_index(analysis.placedHops.size(), hopRow)) {
        context.fatal = true;
        return false;
    }
    catalog::PlacedHop hop{};
    hop.subblockRow = context.subblockRow;
    hop.leafRow = context.leafRow;
    hop.subblockOrdinal = context.subblockOrdinal;
    hop.leafOrdinal = context.leafOrdinal;
    hop.declaredBubbleIndex = context.declaredBubbleIndex;
    hop.tag = source.tag;
    hop.classId = source.classId;
    hop.branchPath = source.branchPath;
    hop.childCount = source.childCount;
    hop.directTargetTag = source.directTargetTag;
    hop.branchPathCount = source.branchPathCount;
    hop.depth = source.depth;
    hop.shape = placed_hop_shape(source.shape);
    hop.complete = true;
    if (!sha256::hash(blob, hop.payloadSha256)) {
        context.fatal = true;
        return false;
    }
    try {
        analysis.placedHops.push_back(hop);
    } catch (...) {
        context.fatal = true;
        return false;
    }

    if (source.shape == tables::PlacedChainShape::config) {
        std::uint32_t occurrenceRow = 0;
        if (!row_index(analysis.placedConfigOccurrences.size(), occurrenceRow)) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
        catalog::PlacedConfigOccurrence occurrence{};
        occurrence.subblockRow = context.subblockRow;
        occurrence.leafRow = context.leafRow;
        occurrence.terminalHopRow = hopRow;
        occurrence.configTag = source.tag;
        occurrence.declaredBubbleIndex = context.declaredBubbleIndex;
        occurrence.branchPath = source.branchPath;
        occurrence.branchPathCount = source.branchPathCount;
        occurrence.complete = true;
        try {
            analysis.placedConfigOccurrences.push_back(occurrence);
        } catch (...) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
        analysis.placedHops[hopRow].configOccurrenceRow = occurrenceRow;
        if (!collect_placed_config(&context, source.tag, blob)) {
            context.leafComplete = false;
            return false;
        }
        return true;
    }

    if (source.shape != tables::PlacedChainShape::bareObjectList) {
        return true;
    }

    std::uint32_t targetRow = 0;
    if (!row_index(analysis.placedBareTargets.size(), targetRow)) {
        analysis.placedHops.pop_back();
        context.fatal = true;
        return false;
    }
    catalog::PlacedBareTarget target{};
    target.subblockRow = context.subblockRow;
    target.leafRow = context.leafRow;
    target.sourceHopRow = hopRow;
    target.declaredBubbleIndex = context.declaredBubbleIndex;
    target.targetTag = source.directTargetTag;
    target.expectedTargetClass = tables::kAuthoredPlacementListClass;
    std::uint32_t targetClass = 0;
    if (!read_tag(*context.build, source.directTargetTag, context.build->chain, targetClass)) {
        target.status = catalog::PlacedBareTargetStatus::unreadableTarget;
        context.leafComplete = false;
        analysis.readComplete = false;
        if (context.build->failed || cancelled(context.build->cancel)) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
    } else {
        target.targetClass = targetClass;
        target.targetLogicalSize = context.build->chain.size();
        if (!sha256::hash(context.build->chain, target.targetPayloadSha256)) {
            analysis.placedHops.pop_back();
            context.fatal = true;
            return false;
        }
        if (targetClass != tables::kAuthoredPlacementListClass) {
            target.status = catalog::PlacedBareTargetStatus::targetClassMismatch;
            context.leafComplete = false;
            analysis.readComplete = false;
        } else {
            target.status = catalog::PlacedBareTargetStatus::completeStructuralEdge;
            if (!internal::collect_authored_placements(analysis.authored,
                                                       context.build->chain,
                                                       source.directTargetTag,
                                                       context.declaredBubbleIndex)) {
                context.leafComplete = false;
                analysis.readComplete = false;
            }
        }
    }
    try {
        analysis.placedBareTargets.push_back(target);
    } catch (...) {
        analysis.placedHops.pop_back();
        context.fatal = true;
        return false;
    }
    analysis.placedHops[hopRow].bareTargetRow = targetRow;
    return true;
}

/** Follows every authored branch and retains every exact path-specific row. */
[[nodiscard]] bool follow_handle(BuildContext& context,
                                 Analysis& analysis,
                                 std::uint32_t handle,
                                 std::uint32_t registryKey,
                                 std::int32_t declaredBubbleIndex,
                                 std::uint32_t subblockRow,
                                 std::uint32_t leafRow,
                                 std::uint32_t subblockOrdinal,
                                 std::uint32_t leafOrdinal) noexcept {
    if (leafRow >= analysis.placedLeaves.size()) {
        return false;
    }
    PlacedChainContext walkContext{&context,
                                   &analysis,
                                   registryKey,
                                   declaredBubbleIndex,
                                   subblockRow,
                                   leafRow,
                                   subblockOrdinal,
                                   leafOrdinal};
    tables::PlacedChainObservation observation{};
    const bool walked = tables::visit_placed_chain_records(handle,
                                                           &placed_chain_reader,
                                                           &walkContext,
                                                           &collect_placed_chain_record,
                                                           &walkContext,
                                                           observation);
    catalog::PlacedLeaf& leaf = analysis.placedLeaves[leafRow];
    const std::size_t hopCount = analysis.placedHops.size() - leaf.firstHop;
    const std::size_t configCount =
        analysis.placedConfigOccurrences.size() - leaf.firstConfigOccurrence;
    const std::size_t bareCount = analysis.placedBareTargets.size() - leaf.firstBareTarget;
    if (!row_index(hopCount, leaf.hopCount) || !row_index(configCount, leaf.configOccurrenceCount)
        || !row_index(bareCount, leaf.bareTargetCount) || walkContext.fatal) {
        return false;
    }
    leaf.complete = walked && walkContext.leafComplete && observation.hopCount == leaf.hopCount
                    && observation.bareTargetCount == leaf.bareTargetCount;
    if (!leaf.complete) {
        analysis.readComplete = false;
    }
    return true;
}

/** @return True when the placed class definition marks its objects as network replicated. */
[[nodiscard]] bool class_replicates(BuildContext& context, std::uint32_t classTag) noexcept {
    const auto cached = context.classReplication.find(classTag);
    if (cached != context.classReplication.end()) {
        return cached->second;
    }
    std::uint32_t classId = 0;
    tables::PlacedClassDefinition definition{};
    const bool replicated = read_tag(context, classTag, context.classBytes, classId)
                            && classId == tables::kPlacedClassDefinitionClass
                            && tables::placed_class_definition(context.classBytes, definition)
                            && definition.networkReplicated;
    try {
        context.classReplication.emplace(classTag, replicated);
    } catch (...) {
        // A missed cache entry costs one more read, nothing else.
    }
    return replicated;
}

/** Counts the authored placements the game replicates: entry flag bit 0 clear, class bit set. */
[[nodiscard]] std::uint32_t count_replicated_placements(BuildContext& context,
                                                        const Analysis& analysis) noexcept {
    std::uint32_t count = 0;
    for (const internal::RawAuthoredPlacement& placement : analysis.authored.placements) {
        if ((placement.placementFlagsRaw & tables::kAuthoredPlacementNoReplicationBit) == 0
            && class_replicates(context, placement.classListTag)) {
            ++count;
        }
    }
    return count;
}

} // namespace

/** Reads one object layout and its reachable descriptor/config records. */
bool analyze_object(BuildContext& context,
                    const tables::Placement& placement,
                    Analysis& output) noexcept {
    output = {};
    tables::Array slots{};
    if (!tables::object_slots(placement.objectBytes, slots) || slots.count > kSlotCapacity) {
        return false;
    }
    try {
        output.slots.reserve(static_cast<std::size_t>(slots.count));
        for (std::uint64_t index = 0; index < slots.count; ++index) {
            tables::Slot slot{};
            if (!tables::object_slot_at(placement.objectBytes, slots, index, slot) || slot.type == 0
                || slot.type > state::build_data::scenarios::kMaximumSlotType) {
                return false;
            }
            output.slots.push_back({slot.nameHash, static_cast<std::uint16_t>(slot.type)});
        }
    } catch (...) {
        return false;
    }

    tables::Array bubbles{};
    if (!tables::object_bubbles(placement.objectBytes, bubbles)
        || bubbles.count > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    try {
        output.placedSubblocks.reserve(static_cast<std::size_t>(bubbles.count));
    } catch (...) {
        return false;
    }
    for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count; ++bubbleIndex) {
        tables::ObjectBubble bubble{};
        std::uint32_t subblockRow = 0;
        std::uint32_t firstLeaf = 0;
        if (!tables::object_bubble_at(placement.objectBytes, bubbles, bubbleIndex, bubble)
            || bubbleIndex > (std::numeric_limits<std::uint32_t>::max)()
            || !row_index(output.placedSubblocks.size(), subblockRow)
            || !row_index(output.placedLeaves.size(), firstLeaf)) {
            return false;
        }
        catalog::PlacedSubblock subblock{};
        subblock.subblockOrdinal = static_cast<std::uint32_t>(bubbleIndex);
        subblock.declaredBubbleIndex = bubble.bubbleIndex;
        subblock.firstLeaf = firstLeaf;
        subblock.sourceOffset = bubble.sourceOffset;
        try {
            output.placedSubblocks.push_back(subblock);
            if (bubble.handleCount
                > (std::numeric_limits<std::uint32_t>::max)() - output.placedLeaves.size()) {
                return false;
            }
            output.placedLeaves.reserve(output.placedLeaves.size()
                                        + static_cast<std::size_t>(bubble.handleCount));
        } catch (...) {
            return false;
        }
        bool subblockComplete = true;
        for (std::uint64_t leafOrdinal = 0; leafOrdinal < bubble.handleCount; ++leafOrdinal) {
            std::uint32_t handle = 0;
            std::uint32_t leafRow = 0;
            catalog::PlacedLeaf leaf{};
            if (!tables::object_placed_handle_at(placement.objectBytes, bubble, leafOrdinal, handle)
                || leafOrdinal > (std::numeric_limits<std::uint32_t>::max)()
                || !row_index(output.placedLeaves.size(), leafRow)
                || !row_index(output.placedHops.size(), leaf.firstHop)
                || !row_index(output.placedConfigOccurrences.size(), leaf.firstConfigOccurrence)
                || !row_index(output.placedBareTargets.size(), leaf.firstBareTarget)) {
                return false;
            }
            leaf.subblockRow = subblockRow;
            leaf.subblockOrdinal = static_cast<std::uint32_t>(bubbleIndex);
            leaf.leafOrdinal = static_cast<std::uint32_t>(leafOrdinal);
            leaf.declaredBubbleIndex = bubble.bubbleIndex;
            leaf.rootTag = handle;
            leaf.sourceOffset = static_cast<std::uint64_t>(bubble.handleDataOffset)
                                + leafOrdinal * tables::kObjectPlacedHandleStride;
            try {
                output.placedLeaves.push_back(leaf);
            } catch (...) {
                return false;
            }
            if (!follow_handle(context,
                               output,
                               handle,
                               placement.objectKey,
                               bubble.bubbleIndex,
                               subblockRow,
                               leafRow,
                               static_cast<std::uint32_t>(bubbleIndex),
                               static_cast<std::uint32_t>(leafOrdinal))) {
                return false;
            }
            subblockComplete = subblockComplete && output.placedLeaves[leafRow].complete;
        }
        const std::size_t leafCount = output.placedLeaves.size() - firstLeaf;
        if (!row_index(leafCount, output.placedSubblocks[subblockRow].leafCount)) {
            return false;
        }
        output.placedSubblocks[subblockRow].complete = subblockComplete;
    }
    if (!row_index(output.observedConfigs.size(), output.configCount)) {
        return false;
    }
    output.replicatedPlacementCount = count_replicated_placements(context, output);
    std::sort(output.descriptors.begin(),
              output.descriptors.end(),
              [](const AnalysisDescriptor& firstRow, const AnalysisDescriptor& secondRow) noexcept {
                  const tables::SlotDescriptor& first = firstRow.descriptor;
                  const tables::SlotDescriptor& second = secondRow.descriptor;
                  if (first.slotIndex != second.slotIndex) {
                      return first.slotIndex < second.slotIndex;
                  }
                  if (first.componentClass != second.componentClass) {
                      return first.componentClass < second.componentClass;
                  }
                  if (first.senseSchema != second.senseSchema) {
                      return first.senseSchema < second.senseSchema;
                  }
                  if (first.authSchema != second.authSchema) {
                      return first.authSchema < second.authSchema;
                  }
                  if (first.configTag != second.configTag) {
                      return first.configTag < second.configTag;
                  }
                  return first.descriptorOffset < second.descriptorOffset;
              });
    return true;
}

} // namespace sunrise::client::content::activity::scriptables::worker_internal
