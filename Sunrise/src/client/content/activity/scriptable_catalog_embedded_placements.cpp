#include "scriptable_catalog_embedded_placements.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../../../middleware/content/packages/tables/container_placement_reader.h"
#include "../../../middleware/content/packages/tables/descriptor_embedded_placement_reader.h"
#include "scriptable_catalog_inline_names.h"

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

namespace catalog = state::build_data::scriptables;
namespace package_reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Hard bounds keep malformed package counts from growing the process-only graph without limit. */
constexpr std::size_t kLinkCapacity = 262'144;
constexpr std::size_t kPlacementCapacity = 262'144;
constexpr std::uint64_t kPlacementsPerDescriptorCapacity = 4'096;

struct Request final {
    std::uint32_t configTag{};
    std::uint32_t descriptorRow{};
};

struct ClassResult final {
    std::uint8_t objectType{};
    bool read{};
};

struct BuildContext final {
    const package_reader::Source* source{};
    package_reader::Scratch* scratch{};
    catalog::Snapshot* output{};
    EmbeddedPlacementCancelCheck cancel{};
    std::vector<std::byte> configBytes{};
    std::vector<std::byte> classBytes{};
    std::unordered_map<std::uint32_t, ClassResult> classes{};
    bool failed{};
};

[[nodiscard]] bool cancelled(const BuildContext& context) noexcept {
    return context.cancel != nullptr && context.cancel();
}

/** Reads one reached tag and keeps all valid inline-name evidence in its blob. */
[[nodiscard]] bool read_tag(BuildContext& context,
                            std::uint32_t tag,
                            std::vector<std::byte>& output,
                            std::uint32_t& classId) noexcept {
    if (!package_reader::read_tag(*context.source, *context.scratch, tag, output, classId)) {
        return false;
    }
    if (!collect_inline_name_evidence(*context.output, output)) {
        context.failed = true;
        return false;
    }
    return true;
}

/** @return The exact slot and object owning one retained descriptor row. */
[[nodiscard]] bool descriptor_owner(const catalog::Snapshot& source,
                                    std::uint32_t descriptorRow,
                                    const catalog::Descriptor*& descriptor,
                                    const catalog::Slot*& slot,
                                    const catalog::Object*& object) noexcept {
    descriptor = nullptr;
    slot = nullptr;
    object = nullptr;
    if (descriptorRow >= source.descriptors.size()) {
        return false;
    }
    descriptor = &source.descriptors[descriptorRow];
    if (descriptor->slotRow >= source.slots.size()) {
        return false;
    }
    slot = &source.slots[descriptor->slotRow];
    if (slot->objectRow >= source.objects.size() || slot->slotType != 4) {
        return false;
    }
    object = &source.objects[slot->objectRow];
    const std::size_t firstDescriptor = slot->firstDescriptor;
    const std::size_t descriptorCount = slot->descriptorCount;
    const std::size_t firstSlot = object->firstSlot;
    const std::size_t slotCount = object->slotCount;
    return firstDescriptor <= descriptorRow
           && descriptorCount <= source.descriptors.size() - firstDescriptor
           && descriptorRow - firstDescriptor < descriptorCount && firstSlot <= descriptor->slotRow
           && slotCount <= source.slots.size() - firstSlot
           && descriptor->slotRow - firstSlot < slotCount;
}

/** Reconstructs the exact validated table-reader input from the catalog identity. */
[[nodiscard]] tables::SlotDescriptor reader_descriptor(const catalog::Descriptor& descriptor,
                                                       const catalog::Slot& slot) noexcept {
    tables::SlotDescriptor output{};
    output.configTag = descriptor.configTag;
    output.componentClass = descriptor.componentClass;
    output.senseSchema = descriptor.senseSchema;
    output.authSchema = descriptor.authSchema;
    output.descriptorOffset = descriptor.descriptorOffset;
    output.bubbleIndex = descriptor.bubbleIndex;
    output.slotType = slot.slotType;
    output.slotIndex = slot.slotIndex;
    return output;
}

/** Reads one exact placed class once and caches both success and failure. */
[[nodiscard]] ClassResult class_result(BuildContext& context, std::uint32_t tag) {
    const auto found = context.classes.find(tag);
    if (found != context.classes.end()) {
        return found->second;
    }
    ClassResult result{};
    std::uint32_t classId = 0;
    tables::PlacedClassDefinition definition{};
    result.read = read_tag(context, tag, context.classBytes, classId)
                  && classId == tables::kPlacedClassDefinitionClass
                  && tables::placed_class_definition(context.classBytes, definition);
    if (result.read) {
        result.objectType = definition.objectType;
    }
    context.classes.emplace(tag, result);
    return result;
}

/** Copies one validated native row into the bounded catalog candidate set. */
void append_candidate(BuildContext& context,
                      catalog::EmbeddedPlacementLink& link,
                      std::uint32_t linkRow,
                      std::uint32_t entryIndex,
                      std::size_t sourceOffset,
                      const tables::DescriptorEmbeddedPlacement& source) {
    catalog::EmbeddedPlacement destination{};
    destination.linkRow = linkRow;
    destination.entryIndex = entryIndex;
    destination.sourceOffset = sourceOffset;
    destination.classListTag = source.classListTag;
    destination.nameHash = source.nameHash;
    destination.identifier = source.identifier;
    destination.auxiliaryRelative = source.auxiliaryRelative;
    destination.auxiliaryOffset = source.auxiliaryOffset;
    destination.rotation = source.rotation;
    destination.position = source.position;
    destination.fourthLane = source.fourthLane;
    destination.replicationByte = source.replicationByte;
    destination.gameworldByte = source.gameworldByte;
    destination.hasAuxiliary = source.hasAuxiliary;
    const ClassResult resolved = class_result(context, source.classListTag);
    destination.objectType = resolved.objectType;
    destination.objectTypeRead = resolved.read;
    context.output->embeddedPlacements.push_back(destination);
    ++link.candidateCount;
    ++context.output->embeddedPlacementDiagnostics.readPlacements;
    if (!resolved.read) {
        ++context.output->embeddedPlacementDiagnostics.unresolvedClassDefinitions;
    }
}

/** Appends one descriptor link and every bounded, independently validated native candidate row. */
[[nodiscard]] bool
append_link(BuildContext& context, std::uint32_t descriptorRow, bool configRead) {
    catalog::EmbeddedPlacementDiagnostics& diagnostics =
        context.output->embeddedPlacementDiagnostics;
    if (context.output->embeddedPlacementLinks.size() >= kLinkCapacity) {
        ++diagnostics.droppedLinks;
        diagnostics.complete = false;
        return true;
    }

    const catalog::Descriptor* descriptor = nullptr;
    const catalog::Slot* slot = nullptr;
    const catalog::Object* object = nullptr;
    catalog::EmbeddedPlacementLink link{};
    link.descriptorRow = descriptorRow;
    link.firstCandidate = static_cast<std::uint32_t>(context.output->embeddedPlacements.size());
    if (!descriptor_owner(*context.output, descriptorRow, descriptor, slot, object)) {
        ++diagnostics.malformedDescriptors;
        diagnostics.complete = false;
    } else {
        link.slotRow = descriptor->slotRow;
        link.objectRow = slot->objectRow;
    }

    const std::uint32_t linkRow =
        static_cast<std::uint32_t>(context.output->embeddedPlacementLinks.size());
    context.output->embeddedPlacementLinks.push_back(link);
    catalog::EmbeddedPlacementLink& retained = context.output->embeddedPlacementLinks.back();
    if (descriptorRow < context.output->descriptors.size()) {
        context.output->descriptors[descriptorRow].embeddedPlacementLinkRow = linkRow;
    }
    if (descriptor == nullptr || slot == nullptr || object == nullptr) {
        return true;
    }
    if (!configRead) {
        ++diagnostics.unreadConfigurations;
        diagnostics.complete = false;
        return true;
    }

    tables::DescriptorEmbeddedPlacementArray array{};
    if (!tables::descriptor_embedded_placements(
            context.configBytes, reader_descriptor(*descriptor, *slot), array)) {
        ++diagnostics.malformedDescriptors;
        diagnostics.complete = false;
        return true;
    }
    retained.declaredPlacementCount = array.count;
    retained.arrayDataOffset = array.dataOffset;
    retained.complete = true;
    if (array.count == 0) {
        ++diagnostics.emptyDescriptors;
        return true;
    }

    const std::uint64_t boundedCount = (std::min)(array.count, kPlacementsPerDescriptorCapacity);
    const std::uint64_t available = kPlacementCapacity - context.output->embeddedPlacements.size();
    const std::uint64_t visitCount = (std::min)(boundedCount, available);
    if (visitCount != array.count) {
        diagnostics.droppedPlacements += array.count - visitCount;
        diagnostics.complete = false;
        retained.complete = false;
    }
    for (std::uint64_t index = 0; index < visitCount; ++index) {
        if (cancelled(context)) {
            diagnostics.complete = false;
            retained.complete = false;
            return false;
        }
        tables::DescriptorEmbeddedPlacement source{};
        if (!tables::descriptor_embedded_placement_at(context.configBytes, array, index, source)) {
            ++diagnostics.malformedPlacements;
            diagnostics.complete = false;
            retained.complete = false;
            continue;
        }
        const std::size_t sourceOffset =
            array.dataOffset + static_cast<std::size_t>(index) * tables::kAuthoredPlacementStride;
        append_candidate(
            context, retained, linkRow, static_cast<std::uint32_t>(index), sourceOffset, source);
    }
    return true;
}

} // namespace

/** Appends every bounded native placement array carried by a reached type-4 slot descriptor. */
bool append_embedded_placements(const package_reader::Source& source,
                                package_reader::Scratch& scratch,
                                catalog::Snapshot& output,
                                EmbeddedPlacementCancelCheck cancel) noexcept {
    output.embeddedPlacementLinks.clear();
    output.embeddedPlacements.clear();
    output.embeddedPlacementDiagnostics = {};
    output.embeddedPlacementDiagnostics.complete = true;
    for (catalog::Descriptor& descriptor : output.descriptors) {
        descriptor.embeddedPlacementLinkRow = catalog::kNoRow;
    }
    try {
        std::vector<Request> requests{};
        requests.reserve(output.descriptors.size());
        for (std::size_t row = 0; row < output.descriptors.size(); ++row) {
            if (cancel != nullptr && cancel()) {
                output.embeddedPlacementDiagnostics.complete = false;
                return false;
            }
            const catalog::Descriptor& descriptor = output.descriptors[row];
            if (descriptor.componentClass != tables::kDescriptorEmbeddedPlacementDescriptorClass
                || descriptor.slotRow >= output.slots.size()
                || output.slots[descriptor.slotRow].slotType
                       != tables::kDescriptorEmbeddedPlacementSlotType) {
                continue;
            }
            ++output.embeddedPlacementDiagnostics.applicableDescriptors;
            requests.push_back({descriptor.configTag, static_cast<std::uint32_t>(row)});
        }
        std::sort(
            requests.begin(), requests.end(), [](const Request& first, const Request& second) {
                return first.configTag != second.configTag
                           ? first.configTag < second.configTag
                           : first.descriptorRow < second.descriptorRow;
            });
        output.embeddedPlacementLinks.reserve((std::min)(requests.size(), kLinkCapacity));
        output.embeddedPlacements.reserve((std::min)(requests.size(), kPlacementCapacity));
        BuildContext context{&source, &scratch, &output, cancel};
        context.classes.reserve(requests.size());
        for (std::size_t first = 0; first < requests.size();) {
            if (cancelled(context)) {
                output.embeddedPlacementDiagnostics.complete = false;
                return false;
            }
            std::size_t last = first + 1;
            while (last < requests.size()
                   && requests[last].configTag == requests[first].configTag) {
                ++last;
            }
            std::uint32_t classId = 0;
            const bool configRead =
                read_tag(context, requests[first].configTag, context.configBytes, classId)
                && classId == tables::kPlacedObjectClass;
            if (context.failed) {
                return false;
            }
            for (std::size_t row = first; row < last; ++row) {
                if (cancelled(context)) {
                    return false;
                }
                if (!append_link(context, requests[row].descriptorRow, configRead)) {
                    return false;
                }
            }
            first = last;
        }
        if (cancelled(context)) {
            output.embeddedPlacementDiagnostics.complete = false;
            return false;
        }
        return !context.failed;
    } catch (...) {
        output.embeddedPlacementDiagnostics.complete = false;
        return false;
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
