#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "../../../middleware/content/packages/reader/typed_array.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808000.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808081.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808088.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809b.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809f.h"
#include "activity_sdk_actor_rsat_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
namespace {

namespace generated = middleware::content::packages::schema::generated;

/** The canonical extraction count ceiling. */
constexpr std::int64_t kMaximumArrayCount = 1'000'000;
/** An actor definition reaches its state-machine definition through a placed object. */
constexpr std::uint32_t kPlacedObjectClass = generated::SEntityConfig::kClass;

/** Reads one package array whose element class is not checked, only its marker and count. */
[[nodiscard]] bool untyped_array(std::span<const std::byte> blob,
                                 std::size_t field,
                                 std::size_t stride,
                                 TypedArray& output) noexcept {
    output = {};
    std::int64_t count = 0;
    std::int64_t relative = 0;
    if (!read_value(blob, field, count)
        || !read_value(blob, field + reader::layout::kArrayFieldRelative, relative) || count < 0
        || count > kMaximumArrayCount) {
        return false;
    }
    output.count = static_cast<std::uint32_t>(count);
    output.relative = relative;
    if (count == 0 && relative == 0) {
        return true;
    }
    std::size_t header = 0;
    reader::TypedArrayRows rows{};
    if (!relative_offset(field + reader::layout::kArrayFieldRelative, relative, header)
        || !reader::read_typed_array(blob,
                                     header,
                                     static_cast<std::uint64_t>(count),
                                     reader::kAnyElementClass,
                                     stride,
                                     rows)) {
        return false;
    }
    output.elementClass = rows.elementClass;
    if (!to_u32(header, output.headerOffset) || !to_u32(rows.data, output.dataOffset)) {
        return false;
    }
    output.typed = true;
    return true;
}

/** Every aligned word in the package tag range; a reference field holds its tag verbatim. */
void tag_shaped_words(std::span<const std::byte> blob, std::vector<std::uint32_t>& output) {
    // Package tags occupy this half-open range.
    constexpr std::uint32_t kFirstTag = middleware::content::packages::reader::layout::kTagBase;
    constexpr std::uint32_t kLastTag = 0x81C00000U;
    output.clear();
    for (std::size_t offset = 0; offset + 4U <= blob.size(); offset += 4U) {
        std::uint32_t word = 0;
        std::memcpy(&word, blob.data() + offset, sizeof word);
        if (word >= kFirstTag && word < kLastTag
            && std::find(output.begin(), output.end(), word) == output.end()) {
            output.push_back(word);
        }
    }
}

/** The state-machine definition lists its groups; each group names its states. */
constexpr std::size_t kStateMachineGroupArrayOffset =
    offsetof(generated::ActorStateMachineDefinition, abilities);
constexpr std::size_t kStateMachineGroupStride = sizeof(generated::StateMachineGroup);
constexpr std::size_t kStateMachineGroupHashOffset =
    offsetof(generated::StateMachineGroup, groupHash);
constexpr std::size_t kStateMachineGroupNamesOffset =
    offsetof(generated::StateMachineGroup, typeNames);
constexpr std::size_t kStateNameStride = sizeof(generated::SU32HashElement);

/** Appends the `state_machine` group's names from one definition, in authored order. */
[[nodiscard]] bool state_machine_names(std::span<const std::byte> definition,
                                       std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    TypedArray groups{};
    if (!untyped_array(
            definition, kStateMachineGroupArrayOffset, kStateMachineGroupStride, groups)) {
        return false;
    }
    bool found = false;
    for (std::uint32_t index = 0; index < groups.count; ++index) {
        const std::size_t element = static_cast<std::size_t>(groups.dataOffset)
                                    + static_cast<std::size_t>(index) * kStateMachineGroupStride;
        std::uint32_t groupHash = 0;
        if (!read_value(definition, element + kStateMachineGroupHashOffset, groupHash)) {
            return false;
        }
        if (groupHash != format::kActorStateMachineGroupHash) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        TypedArray names{};
        if (!untyped_array(
                definition, element + kStateMachineGroupNamesOffset, kStateNameStride, names)) {
            return false;
        }
        for (std::uint32_t ordinal = 0; ordinal < names.count; ++ordinal) {
            std::uint32_t nameHash = 0;
            if (!read_value(definition,
                            static_cast<std::size_t>(names.dataOffset)
                                + static_cast<std::size_t>(ordinal) * kStateNameStride,
                            nameHash)) {
                return false;
            }
            output.push_back(nameHash);
        }
    }
    return found;
}

} // namespace

/** Reads the exact `{i64 count, i64 self-relative header}` package array. */
bool typed_array(std::span<const std::byte> blob,
                 std::size_t field,
                 std::uint32_t expectedClass,
                 std::size_t stride,
                 TypedArray& output) noexcept {
    output = {};
    std::int64_t count = 0;
    std::int64_t relative = 0;
    if (!read_value(blob, field, count)
        || !read_value(blob, field + reader::layout::kArrayFieldRelative, relative) || count < 0
        || count > kMaximumArrayCount) {
        return false;
    }
    output.count = static_cast<std::uint32_t>(count);
    output.relative = relative;
    if (count == 0 && relative == 0) {
        return true;
    }

    std::size_t header = 0;
    reader::TypedArrayRows rows{};
    std::uint32_t padding = 0;
    if (!relative_offset(field + reader::layout::kArrayFieldRelative, relative, header)
        || !reader::read_typed_array(
            blob, header, static_cast<std::uint64_t>(count), expectedClass, stride, rows)
        || !read_value(blob, header + reader::layout::TypedArrayHeader::kPadding, padding)
        || padding != 0 || !to_u32(header, output.headerOffset)
        || !to_u32(rows.data, output.dataOffset)) {
        return false;
    }
    output.elementClass = rows.elementClass;
    output.typed = true;
    return true;
}

/**
 * Collects the state names one actor class can be told to enter. The actor definition refers to
 * placed objects, and exactly one of those refers to one state-machine definition. Any other
 * shape emits no rows for the class.
 */
bool collect_state_names(BuildState& state,
                         std::uint32_t actorIndex,
                         std::span<const std::byte> actorBlob) {
    std::vector<std::uint32_t> words{};
    std::vector<std::uint32_t> placedWords{};
    std::vector<std::byte> placed{};
    std::vector<std::byte> definition{};
    std::vector<std::uint32_t> definitions{};
    tag_shaped_words(actorBlob, words);
    for (const std::uint32_t placedTag : words) {
        if (!state.readTag(state.readContext, placedTag, kPlacedObjectClass, placed)) {
            continue;
        }
        tag_shaped_words(placed, placedWords);
        for (const std::uint32_t definitionTag : placedWords) {
            if (std::find(definitions.begin(), definitions.end(), definitionTag)
                    != definitions.end()
                || !state.readTag(state.readContext,
                                  definitionTag,
                                  generated::ActorStateMachineDefinition::kClass,
                                  definition)) {
                continue;
            }
            definitions.push_back(definitionTag);
        }
    }
    if (definitions.size() != 1
        || !state.readTag(state.readContext,
                          definitions.front(),
                          generated::ActorStateMachineDefinition::kClass,
                          definition)) {
        return true;
    }
    std::vector<std::uint32_t> names{};
    if (!state_machine_names(definition, names)) {
        return true;
    }
    // The ordinal counts emitted rows, not authored positions. A skipped name would otherwise
    // leave a gap, and the catalog requires one contiguous run per actor class.
    std::uint32_t ordinal = 0;
    for (const std::uint32_t nameHash : names) {
        if (nameHash == 0 || nameHash == kAbsentTag) {
            continue;
        }
        ActorStateName row{};
        row.actorClassIndex = actorIndex;
        row.definitionTag = definitions.front();
        row.groupHash = format::kActorStateMachineGroupHash;
        row.nameHash = nameHash;
        row.ordinal = ordinal++;
        row.flags = format::kActorStateNameExact;
        state.snapshot.actorStateNames.push_back(row);
    }
    return true;
}

/** Reads and retains one schema exactly once by tag. */
bool schema(BuildState& state, std::uint32_t tag, std::size_t& sourceIndex) noexcept {
    sourceIndex = 0;
    if (is_absent_tag(tag) || is_cancelled(state.cancel, state.cancelContext)) {
        return false;
    }
    const auto found = state.schemaIndexes.find(tag);
    if (found != state.schemaIndexes.end()) {
        sourceIndex = found->second;
        return true;
    }

    std::vector<std::byte> blob{};
    if (!state.readTag(state.readContext, tag, generated::SReplicationSchema::kClass, blob)) {
        return false;
    }
    TypedArray array{};
    if (!typed_array(blob,
                     kSchemaFieldArrayOffset,
                     generated::RsatSchemaField::kClass,
                     kSchemaFieldStride,
                     array)) {
        return false;
    }

    SchemaSource source{};
    if (!format_schema_id(tag, source.row.id)) {
        return false;
    }
    source.row.schemaTag = tag;
    source.row.schemaClass = generated::SReplicationSchema::kClass;
    source.row.fieldCount = array.count;
    source.row.fieldArrayOffset = kSchemaFieldArrayOffset;
    source.row.fieldArrayRelative = array.relative;
    source.row.fieldArrayHeaderOffset = array.headerOffset;
    source.row.fieldArrayDataOffset = array.dataOffset;
    source.row.fieldElementClass = array.elementClass;
    if (array.typed) {
        source.row.flags |= format::kRsatSchemaTypedFieldArray;
    }
    if (array.count != 0) {
        if (!read_value(blob, array.dataOffset, source.row.firstFieldRuntimeGate)
            || !read_value(
                blob, array.dataOffset + kFieldRawU32At10Offset, source.row.firstFieldRawU32At10)) {
            return false;
        }
        if (source.row.firstFieldRuntimeGate != format::kAbsentIndex) {
            source.row.flags |= format::kRsatSchemaDynamicPresenceEligible;
        }
    }
    try {
        source.fields.reserve(array.count);
        for (std::uint32_t ordinal = 0; ordinal < array.count; ++ordinal) {
            const std::size_t offset = static_cast<std::size_t>(array.dataOffset)
                                       + static_cast<std::size_t>(ordinal) * kSchemaFieldStride;
            RsatField field{};
            std::copy_n(blob.data() + offset, field.rawRow.size(), field.rawRow.begin());
            source.fields.push_back(field);
        }
        sourceIndex = state.schemaSources.size();
        state.schemaSources.push_back(std::move(source));
        state.schemaIndexes.emplace(tag, sourceIndex);
        return true;
    } catch (...) {
        return false;
    }
}

/** Reads one installed SObject RSAT and retains its complete ordered component layout. */
bool sobject_rsat(BuildState& state, std::uint32_t tag) noexcept {
    if (is_absent_tag(tag) || is_cancelled(state.cancel, state.cancelContext)
        || state.snapshot.sobjectRsats.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    std::vector<std::byte> blob{};
    TypedArray array{};
    SobjectRsat row{};
    if (!state.readTag(state.readContext, tag, kActorRsatClass, blob)
        || !read_value(blob, kActorRsatReverseDefinitionOffset, row.reverseDefinitionTag)
        || !typed_array(blob,
                        kRsatDescriptorArrayOffset,
                        generated::RsatDescriptor::kClass,
                        kDescriptorStride,
                        array)
        || state.snapshot.sobjectRsatDescriptors.size()
               > (std::numeric_limits<std::uint32_t>::max)() - array.count) {
        return false;
    }
    row.rsatTag = tag;
    row.descriptorArrayOffset = kRsatDescriptorArrayOffset;
    row.descriptorArrayRelative = array.relative;
    row.descriptorArrayHeaderOffset = array.headerOffset;
    row.descriptorArrayDataOffset = array.dataOffset;
    row.descriptorElementClass = array.elementClass;
    row.provenance = format::ActorSemanticProvenance::packageField;
    row.descriptors.first =
        static_cast<std::uint32_t>(state.snapshot.sobjectRsatDescriptors.size());
    row.descriptors.count = array.count;
    row.flags = format::kSobjectRsatExact;
    const std::uint32_t rsatIndex = static_cast<std::uint32_t>(state.snapshot.sobjectRsats.size());
    std::uint32_t tailOrdinal = 0;
    for (std::uint32_t ordinal = 0; ordinal < array.count; ++ordinal) {
        const std::size_t offset = static_cast<std::size_t>(array.dataOffset)
                                   + static_cast<std::size_t>(ordinal) * kDescriptorStride;
        SobjectRsatDescriptor descriptor{};
        descriptor.rsatIndex = rsatIndex;
        descriptor.descriptorOrdinal = ordinal;
        if (!to_u32(offset, descriptor.descriptorOffset)) {
            return false;
        }
        std::copy_n(blob.data() + offset, descriptor.rawRow.size(), descriptor.rawRow.begin());
        std::memcpy(
            &descriptor.componentTag, descriptor.rawRow.data(), sizeof descriptor.componentTag);
        std::memcpy(&descriptor.schemaTag,
                    descriptor.rawRow.data() + kDescriptorSchemaOffset,
                    sizeof descriptor.schemaTag);
        std::size_t schemaSourceIndex = 0;
        if (!schema(state, descriptor.schemaTag, schemaSourceIndex)) {
            return false;
        }
        const RsatSchema& schemaRow = state.schemaSources[schemaSourceIndex].row;
        descriptor.schemaFieldCount = schemaRow.fieldCount;
        descriptor.schemaFirstFieldRuntimeGate = schemaRow.firstFieldRuntimeGate;
        if ((schemaRow.flags & format::kRsatSchemaDynamicPresenceEligible) != 0) {
            descriptor.flags |= format::kSobjectRsatDescriptorDynamicPresenceEligible;
            descriptor.dynamicPresenceTailOrdinal = tailOrdinal++;
        }
        state.snapshot.sobjectRsatDescriptors.push_back(descriptor);
    }
    row.dynamicPresenceTailCount = tailOrdinal;
    state.snapshot.sobjectRsats.push_back(row);
    return true;
}

/** Reads one package row and confirms its physical class. */
bool package_read(void* opaque,
                  std::uint32_t tag,
                  std::uint32_t expectedClass,
                  std::vector<std::byte>& output) noexcept {
    output.clear();
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PackageReadContext*>(opaque);
    const auto prefetched = context.prefetched.find(tag);
    if (prefetched != context.prefetched.end()) {
        if (prefetched->second.classId != expectedClass) {
            return false;
        }
        try {
            output = prefetched->second.bytes;
            return true;
        } catch (...) {
            output.clear();
            return false;
        }
    }
    std::uint32_t actualClass = 0;
    return context.source != nullptr && context.scratch != nullptr
           && reader::read_tag_class(*context.source, *context.scratch, tag, actualClass)
           && actualClass == expectedClass
           && reader::read_tag(*context.source, *context.scratch, tag, output, actualClass);
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
