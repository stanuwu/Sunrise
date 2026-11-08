#include "activity_sdk_squad_inventory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "../../../middleware/content/packages/tables/authored_placement_reader.h"
#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include "activity_sdk_squad_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {
namespace {

namespace tables = middleware::content::packages::tables;
namespace topology = topology_inventory;

/** One extracted config path before it is expanded across scenario occurrences. */
struct RawConfigPath final {
    std::uint32_t configTag{};
    std::uint32_t ordinal{};
    std::int32_t declaredBubbleIndex{};
};

/** One parsed object-list entry before its scenario occurrence is attached. */
struct RawPlacement final {
    std::uint32_t placementOrdinal{};
    std::uint64_t placedEntryIdentity{};
    std::array<std::uint32_t, 3> positionBits{};
    std::uint32_t actorDefinitionTag{format::kAbsentIndex};
    std::uint64_t sourceOffset{};
    std::array<std::uint32_t, 4> quaternionBits{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::int64_t auxiliaryRelative{};
};

/** One path-specific bare object-list target. */
struct RawPlacementPath final {
    std::uint32_t objectListTag{};
    std::uint32_t ordinal{};
    std::int32_t declaredBubbleIndex{};
    std::vector<RawPlacement> placements{};
};

/** Exact placed-chain terminals owned by one object definition. */
struct ObjectPaths final {
    std::vector<RawConfigPath> configs{};
    std::vector<RawPlacementPath> placements{};
};

/** Mutable extraction state shared by placed-chain callbacks. */
struct Collector final {
    const topology::Snapshot* topology{};
    TagReader reader{};
    void* readerContext{};
    Facts* facts{};
    std::vector<ObjectPaths>* objects{};
    std::unordered_map<std::uint32_t, std::size_t> spawners{};
    std::unordered_map<std::uint32_t, std::size_t> rules{};
    std::unordered_map<std::string, std::size_t> descriptors{};
};

/** Context for one object definition's descriptor callback. */
struct DescriptorContext final {
    Collector* collector{};
    std::uint32_t objectIndex{format::kAbsentIndex};
};

/** Context retained while one placed handle is observed. */
struct ChainContext final {
    Collector* collector{};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t nextConfigOrdinal{};
    std::uint32_t nextPlacementOrdinal{};
    std::vector<std::byte> chainBytes{};
};

/** Reads one trivially-copyable package field inside the supplied blob. */
template <typename Value>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, Value& output) noexcept {
    if (offset > blob.size() || sizeof output > blob.size() - offset) {
        output = {};
        return false;
    }
    std::memcpy(&output, blob.data() + offset, sizeof output);
    return true;
}

using detail::descriptor_id;
using detail::format_text;
using detail::text_view;

/** @return True when a float is finite and its raw lane is retained. */
[[nodiscard]] bool position_bits(const std::array<float, 3>& position,
                                 std::array<std::uint32_t, 3>& output) noexcept {
    for (std::size_t lane = 0; lane < position.size(); ++lane) {
        if (!std::isfinite(position[lane])) {
            output = {};
            return false;
        }
        output[lane] = std::bit_cast<std::uint32_t>(position[lane]);
    }
    return true;
}

/** @return True when the exact authored bubble rule includes one state. */
[[nodiscard]] bool bubble_applies(std::int32_t declared, std::uint32_t entryIndex) noexcept {
    return declared == tables::kGlobalBubbleIndex
           || (declared >= 0 && static_cast<std::uint32_t>(declared) == entryIndex);
}

/** Inserts one exact parsed spawner or rejects a repeated mismatch. */
[[nodiscard]] bool merge_spawner(Collector& collector, SpawnerFact value) {
    const auto found = collector.spawners.find(value.configTag);
    if (found == collector.spawners.end()) {
        const std::size_t row = collector.facts->spawners.size();
        collector.facts->spawners.push_back(std::move(value));
        collector.spawners.emplace(collector.facts->spawners[row].configTag, row);
        return true;
    }
    return found->second < collector.facts->spawners.size()
           && detail::same_spawner(collector.facts->spawners[found->second], value);
}

/** Inserts one exact parsed rule or rejects a repeated mismatch. */
[[nodiscard]] bool merge_rule(Collector& collector, RuleFact value) {
    const auto found = collector.rules.find(value.configTag);
    if (found == collector.rules.end()) {
        const std::size_t row = collector.facts->rules.size();
        collector.facts->rules.push_back(std::move(value));
        collector.rules.emplace(collector.facts->rules[row].configTag, row);
        return true;
    }
    return found->second < collector.facts->rules.size()
           && detail::same_rule(collector.facts->rules[found->second], value);
}

/** Inserts one exact descriptor or rejects a repeated mismatch. */
[[nodiscard]] bool merge_descriptor(Collector& collector, DescriptorFact value) {
    const auto found = collector.descriptors.find(value.id);
    if (found == collector.descriptors.end()) {
        const std::size_t row = collector.facts->descriptors.size();
        collector.facts->descriptors.push_back(std::move(value));
        collector.descriptors.emplace(collector.facts->descriptors[row].id, row);
        return true;
    }
    if (found->second >= collector.facts->descriptors.size()) {
        return false;
    }
    const DescriptorFact& row = collector.facts->descriptors[found->second];
    return row.configTag == value.configTag && row.objectIndex == value.objectIndex
           && row.slotIndex == value.slotIndex && row.descriptorOffset == value.descriptorOffset
           && row.componentClass == value.componentClass && row.senseSchema == value.senseSchema
           && row.authSchema == value.authSchema && row.complete == value.complete;
}

/** Collects one config blob's exact descriptor rows for an object definition. */
bool collect_descriptor(void* opaque, const tables::SlotDescriptor& source) noexcept {
    auto& context = *static_cast<DescriptorContext*>(opaque);
    if (context.collector == nullptr || context.collector->topology == nullptr
        || context.collector->facts == nullptr
        || context.objectIndex >= context.collector->topology->objects.size()) {
        return false;
    }
    const topology::Snapshot& topology = *context.collector->topology;
    const topology::Object& object = topology.objects[context.objectIndex];
    if (object.firstSlot > topology.slots.size()
        || object.slotCount > topology.slots.size() - object.firstSlot) {
        return false;
    }
    // A high-bit slot ordinal cannot belong to this lane, which is signed i16.
    // So the row is an unrelated descriptor, not a malformed config array.
    if (source.slotIndex > static_cast<std::uint16_t>((std::numeric_limits<std::int16_t>::max)())) {
        return true;
    }
    const auto first = topology.slots.begin() + object.firstSlot;
    const auto last = first + object.slotCount;
    const auto slot = std::find_if(first, last, [&source](const topology::Slot& row) {
        return row.slotType == source.slotType && row.slotIndex == source.slotIndex;
    });
    if (slot == last) {
        return true;
    }
    DescriptorFact row{};
    row.configTag = source.configTag;
    row.objectIndex = context.objectIndex;
    row.slotIndex = static_cast<std::uint32_t>(slot - topology.slots.begin());
    row.descriptorOffset = source.descriptorOffset;
    row.componentClass = source.componentClass;
    row.senseSchema = source.senseSchema;
    row.authSchema = source.authSchema;
    row.complete = true;
    if (!descriptor_id(topology, row, row.id)) {
        return false;
    }
    try {
        return merge_descriptor(*context.collector, std::move(row));
    } catch (...) {
        return false;
    }
}

/**
 * Collects the config's descriptor without the class-shape checks. The shared native visitor is
 * narrower, because live consumers need class-shaped component and schema fields. Keeping the
 * wider row here stops a mismatched target row from being hidden and upgraded.
 */
[[nodiscard]] bool collect_structural_descriptors(DescriptorContext& context,
                                                  std::span<const std::byte> blob,
                                                  std::uint32_t configTag) noexcept {
    if (context.collector == nullptr || context.collector->topology == nullptr
        || context.objectIndex >= context.collector->topology->objects.size()) {
        return false;
    }
    const topology::Object& object = context.collector->topology->objects[context.objectIndex];
    std::size_t base = 0;
    std::uint32_t ownerKey = 0;
    if (!tables::config_descriptor_offset(blob, configTag, base)
        || !read_value(blob, base + tables::kDescriptorRegistryKeyOffset, ownerKey)
        || ownerKey != object.objectKey) {
        return true;
    }
    std::int16_t signedSlotIndex = -1;
    tables::SlotDescriptor descriptor{};
    descriptor.configTag = configTag;
    descriptor.descriptorOffset = static_cast<std::uint32_t>(base);
    if (!read_value(blob, base + tables::kDescriptorComponentClassOffset, descriptor.componentClass)
        || !read_value(blob, base + tables::kDescriptorBubbleIndexOffset, descriptor.bubbleIndex)
        || !read_value(blob, base + tables::kDescriptorSenseSchemaOffset, descriptor.senseSchema)
        || !read_value(blob, base + tables::kDescriptorAuthSchemaOffset, descriptor.authSchema)
        || !read_value(blob, base + tables::kDescriptorSlotTypeOffset, descriptor.slotType)
        || !read_value(blob, base + tables::kDescriptorSlotIndexOffset, signedSlotIndex)) {
        return false;
    }
    if (signedSlotIndex < 0) {
        return true;
    }
    descriptor.slotIndex = static_cast<std::uint16_t>(signedSlotIndex);
    return collect_descriptor(&context, descriptor);
}

/** Parses and records one path-specific placed config terminal. */
bool collect_config(void* opaque,
                    std::uint32_t configTag,
                    std::span<const std::byte> blob) noexcept {
    auto& context = *static_cast<ChainContext*>(opaque);
    if (context.collector == nullptr || context.collector->facts == nullptr
        || context.collector->objects == nullptr
        || context.objectIndex >= context.collector->objects->size()) {
        return false;
    }
    tables::ComponentPair pair{};
    if (!tables::config_component_pair(blob, configTag, pair)) {
        return false;
    }
    try {
        if (pair.primaryClass == tables::kAuthoredSquadSpawnerPrimaryClass
            && pair.secondaryClass == tables::kAuthoredSquadSpawnerSecondaryClass) {
            SpawnerFact spawner{};
            if (!detail::parse_spawner(configTag, blob, spawner)) {
                return false;
            }
            const bool hasInline = spawner.hasInlinePointSet;
            RuleFact rule = hasInline ? detail::inline_rule(spawner) : RuleFact{};
            if (!merge_spawner(*context.collector, std::move(spawner))
                || (hasInline && !merge_rule(*context.collector, std::move(rule)))) {
                return false;
            }
        } else if (pair.primaryClass == tables::kAuthoredSquadRulePrimaryClass
                   && pair.secondaryClass == tables::kAuthoredSquadRuleSecondaryClass) {
            RuleFact rule{};
            if (!detail::parse_rule(configTag, blob, rule)
                || !merge_rule(*context.collector, std::move(rule))) {
                return false;
            }
        }
        DescriptorContext descriptorContext{context.collector, context.objectIndex};
        if (!collect_structural_descriptors(descriptorContext, blob, configTag)
            || !tables::visit_slot_descriptors(
                blob,
                configTag,
                context.collector->topology->objects[context.objectIndex].objectKey,
                &collect_descriptor,
                &descriptorContext)) {
            return false;
        }
        ObjectPaths& object = (*context.collector->objects)[context.objectIndex];
        object.configs.push_back(
            {configTag, context.nextConfigOrdinal++, context.declaredBubbleIndex});
        return true;
    } catch (...) {
        return false;
    }
}

/** Parses and records one path-specific bare object-list target. */
bool collect_bare_target(void* opaque, std::uint32_t, std::uint32_t targetTag) noexcept {
    auto& context = *static_cast<ChainContext*>(opaque);
    if (context.collector == nullptr || context.collector->reader == nullptr
        || context.collector->objects == nullptr
        || context.objectIndex >= context.collector->objects->size()) {
        return false;
    }
    std::vector<std::byte> bytes{};
    std::uint32_t classId = 0;
    if (!context.collector->reader(context.collector->readerContext, targetTag, bytes, classId)
        || classId != tables::kAuthoredPlacementListClass) {
        return false;
    }
    tables::Array array{};
    if (!tables::authored_placements(bytes, array) || array.count >= format::kAbsentIndex) {
        return false;
    }
    RawPlacementPath path{};
    path.objectListTag = targetTag;
    path.ordinal = context.nextPlacementOrdinal++;
    path.declaredBubbleIndex = context.declaredBubbleIndex;
    try {
        path.placements.reserve(static_cast<std::size_t>(array.count));
        for (std::uint64_t index = 0; index < array.count; ++index) {
            tables::AuthoredPlacement source{};
            std::uint64_t identity = 0;
            RawPlacement row{};
            if (!tables::authored_placement_at(bytes, array, index, source)
                || !tables::authored_placement_identifier_at(bytes, array, index, identity)
                || identity != source.placementIdentifier
                || !position_bits(source.position, row.positionBits)
                || row.positionBits != source.positionBits) {
                return false;
            }
            if (source.classListTag != 0 && source.classListTag != format::kAbsentIndex) {
                context.collector->facts->actorDefinitionTags.push_back(source.classListTag);
            }
            row.placementOrdinal = static_cast<std::uint32_t>(index);
            row.placedEntryIdentity = identity;
            row.actorDefinitionTag = source.classListTag;
            row.sourceOffset = source.sourceOffset;
            row.quaternionBits = source.rotationBits;
            row.uniformScaleBits = source.uniformScaleBits;
            row.nameHash = source.nameHash;
            row.placementFlagsRaw = source.placementFlagsRaw;
            row.auxiliaryRelative = source.auxiliaryRelative;
            path.placements.push_back(row);
        }
        (*context.collector->objects)[context.objectIndex].placements.push_back(std::move(path));
        return true;
    } catch (...) {
        return false;
    }
}

/** Supplies one chain node from the caller-owned package reader. */
bool chain_reader(void* opaque,
                  std::uint32_t tag,
                  std::span<const std::byte>& blob,
                  std::uint32_t& classId) noexcept {
    auto& context = *static_cast<ChainContext*>(opaque);
    blob = {};
    classId = 0;
    if (context.collector == nullptr || context.collector->reader == nullptr
        || !context.collector->reader(
            context.collector->readerContext, tag, context.chainBytes, classId)) {
        return false;
    }
    blob = context.chainBytes;
    return true;
}

/** Validates one object definition and extracts every path-specific terminal once. */
[[nodiscard]] bool collect_object(Collector& collector, std::uint32_t objectIndex) {
    if (collector.topology == nullptr || collector.reader == nullptr || collector.objects == nullptr
        || objectIndex >= collector.topology->objects.size()) {
        return false;
    }
    const topology::Object& object = collector.topology->objects[objectIndex];
    std::vector<std::byte> bytes{};
    std::uint32_t classId = 0;
    if (!collector.reader(collector.readerContext, object.objectTag, bytes, classId)
        || classId != tables::kObjectClass) {
        return false;
    }
    std::uint32_t objectKey = 0;
    tables::Array slots{};
    if (!tables::object_key(bytes, objectKey) || objectKey != object.objectKey
        || !tables::object_slots(bytes, slots) || slots.count != object.slotCount
        || object.firstSlot > collector.topology->slots.size()
        || object.slotCount > collector.topology->slots.size() - object.firstSlot) {
        return false;
    }
    for (std::uint64_t ordinal = 0; ordinal < slots.count; ++ordinal) {
        tables::Slot source{};
        const topology::Slot& expected = collector.topology->slots[object.firstSlot + ordinal];
        if (!tables::object_slot_at(bytes, slots, ordinal, source)
            || expected.objectIndex != objectIndex || expected.slotIndex != ordinal
            || expected.slotType != source.type || expected.nameHash != source.nameHash) {
            return false;
        }
    }
    tables::Array bubbles{};
    if (!tables::object_bubbles(bytes, bubbles)) {
        return false;
    }
    ChainContext chain{&collector, objectIndex};
    for (std::uint64_t subblock = 0; subblock < bubbles.count; ++subblock) {
        tables::ObjectBubble bubble{};
        if (!tables::object_bubble_at(bytes, bubbles, subblock, bubble)) {
            return false;
        }
        chain.declaredBubbleIndex = bubble.bubbleIndex;
        for (std::uint64_t leaf = 0; leaf < bubble.handleCount; ++leaf) {
            std::uint32_t handle = 0;
            tables::PlacedChainObservation observation{};
            if (!tables::object_placed_handle_at(bytes, bubble, leaf, handle)
                || !tables::observe_placed_chain(handle,
                                                 &chain_reader,
                                                 &chain,
                                                 &collect_config,
                                                 &chain,
                                                 &collect_bare_target,
                                                 &chain,
                                                 observation)
                || !observation.complete) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/** Reads exact package facts for every object definition in one closed topology. */
bool collect_facts(const topology::Snapshot& topology,
                   TagReader reader,
                   void* readerContext,
                   Facts& output) noexcept {
    output = {};
    if (!detail::valid_topology(topology) || reader == nullptr) {
        return false;
    }
    try {
        Facts pending{};
        std::vector<ObjectPaths> objects(topology.objects.size());
        Collector collector{&topology, reader, readerContext, &pending, &objects};
        for (std::uint32_t objectIndex = 0; objectIndex < topology.objects.size(); ++objectIndex) {
            if (!collect_object(collector, objectIndex)) {
                return false;
            }
        }
        for (std::uint32_t occurrenceIndex = 0; occurrenceIndex < topology.occurrences.size();
             ++occurrenceIndex) {
            const topology::Occurrence& occurrence = topology.occurrences[occurrenceIndex];
            const topology::State& state = topology.states[occurrence.stateIndex];
            std::string_view occurrenceId{};
            if (!text_view(occurrence.id, occurrenceId)) {
                return false;
            }
            const ObjectPaths& paths = objects[occurrence.objectIndex];
            for (const RawConfigPath& path : paths.configs) {
                if (!bubble_applies(path.declaredBubbleIndex, state.entryIndex)) {
                    continue;
                }
                ConfigOccurrenceFact row{};
                row.configTag = path.configTag;
                row.occurrenceIndex = occurrenceIndex;
                row.complete = true;
                row.objectIndex = occurrence.objectIndex;
                row.pathOrdinal = path.ordinal;
                row.declaredBubbleIndex = path.declaredBubbleIndex;
                if (!format_text(row.id,
                                 "config-occurrence/%08x/%08x/%08x",
                                 static_cast<unsigned>(occurrenceIndex),
                                 static_cast<unsigned>(path.ordinal),
                                 static_cast<unsigned>(path.configTag))) {
                    return false;
                }
                pending.configOccurrences.push_back(std::move(row));
            }
            for (const RawPlacementPath& path : paths.placements) {
                if (!bubble_applies(path.declaredBubbleIndex, state.entryIndex)) {
                    continue;
                }
                for (const RawPlacement& placement : path.placements) {
                    PlacementOccurrenceFact row{};
                    row.occurrenceIndex = occurrenceIndex;
                    row.objectListTag = path.objectListTag;
                    row.placementOrdinal = placement.placementOrdinal;
                    row.placedEntryIdentity = placement.placedEntryIdentity;
                    row.positionBits = placement.positionBits;
                    row.complete = true;
                    row.objectIndex = occurrence.objectIndex;
                    row.pathOrdinal = path.ordinal;
                    row.declaredBubbleIndex = path.declaredBubbleIndex;
                    row.actorDefinitionTag = placement.actorDefinitionTag;
                    row.sourceOffset = placement.sourceOffset;
                    row.quaternionBits = placement.quaternionBits;
                    row.uniformScaleBits = placement.uniformScaleBits;
                    row.nameHash = placement.nameHash;
                    row.placementFlagsRaw = placement.placementFlagsRaw;
                    row.auxiliaryRelative = placement.auxiliaryRelative;
                    if (!format_text(row.id,
                                     "placement-occurrence/%08x/%08x/%08x/%08x",
                                     static_cast<unsigned>(occurrenceIndex),
                                     static_cast<unsigned>(path.ordinal),
                                     static_cast<unsigned>(path.objectListTag),
                                     static_cast<unsigned>(placement.placementOrdinal))) {
                        return false;
                    }
                    pending.placementOccurrences.push_back(std::move(row));
                }
            }
        }
        std::sort(pending.spawners.begin(),
                  pending.spawners.end(),
                  [](const auto& first, const auto& second) {
                      return first.configTag < second.configTag;
                  });
        std::sort(
            pending.rules.begin(), pending.rules.end(), [](const auto& first, const auto& second) {
                return first.configTag < second.configTag;
            });
        std::sort(pending.descriptors.begin(),
                  pending.descriptors.end(),
                  [](const auto& first, const auto& second) { return first.id < second.id; });
        std::sort(pending.configOccurrences.begin(),
                  pending.configOccurrences.end(),
                  [](const auto& first, const auto& second) { return first.id < second.id; });
        std::sort(pending.placementOccurrences.begin(),
                  pending.placementOccurrences.end(),
                  [](const auto& first, const auto& second) { return first.id < second.id; });
        for (const SpawnerFact& spawner : pending.spawners) {
            for (const MemberFact& member : spawner.members) {
                for (const auto& lane : member.candidates) {
                    for (const CandidateFact& candidate : lane) {
                        if (candidate.state == CandidateState::exactPlacement
                            && candidate.actorDefinitionTag != 0
                            && candidate.actorDefinitionTag != format::kAbsentIndex) {
                            pending.actorDefinitionTags.push_back(candidate.actorDefinitionTag);
                        }
                    }
                }
            }
        }
        std::sort(pending.actorDefinitionTags.begin(), pending.actorDefinitionTags.end());
        pending.actorDefinitionTags.erase(
            std::unique(pending.actorDefinitionTags.begin(), pending.actorDefinitionTags.end()),
            pending.actorDefinitionTags.end());
        pending.complete = true;
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

/** Adds the placements a scenario loads that no activity object list carries. */
bool append_external_placements(const topology::Snapshot& topology,
                                const external_placements::Index& index,
                                Facts& facts) noexcept {
    if (index.scenarioRows.empty()) {
        return true;
    }
    try {
        // Only an identity some authored point asks for can ever anchor a squad. Without this
        // the harvest would add one fact per placement per scenario, millions of rows.
        std::set<std::uint64_t> wanted{};
        for (const RuleFact& rule : facts.rules) {
            for (const RulePointFact& point : rule.points) {
                wanted.insert(point.placedEntryIdentity);
            }
        }
        for (const SpawnerFact& spawner : facts.spawners) {
            for (const RulePointFact& point : spawner.inlinePoints) {
                wanted.insert(point.placedEntryIdentity);
            }
        }
        if (wanted.empty()) {
            return true;
        }
        // A point resolves against an external row only where the object's own lists hold none,
        // so a join that already resolves keeps its single match and cannot turn ambiguous.
        std::set<std::pair<std::uint32_t, std::uint64_t>> owned{};
        for (const PlacementOccurrenceFact& placement : facts.placementOccurrences) {
            if (placement.external || placement.occurrenceIndex >= topology.occurrences.size()) {
                continue;
            }
            const std::uint32_t scenarioIndex =
                topology.occurrences[placement.occurrenceIndex].scenarioIndex;
            owned.emplace(scenarioIndex, placement.placedEntryIdentity);
        }
        for (std::uint32_t scenarioIndex = 0; scenarioIndex < topology.scenarios.size();
             ++scenarioIndex) {
            const std::uint32_t scenarioTag = topology.scenarios[scenarioIndex].tag;
            std::uint32_t ordinal = 0;
            for (const external_placements::ScenarioRow& reference :
                 external_placements::scenario_rows(index, scenarioTag)) {
                const std::uint32_t rowOrdinal = ordinal++;
                if (reference.rowIndex >= index.rows.size()) {
                    return false;
                }
                const external_placements::Row& row = index.rows[reference.rowIndex];
                if (!wanted.contains(row.placedEntryIdentity)
                    || owned.contains({scenarioIndex, row.placedEntryIdentity})) {
                    continue;
                }
                PlacementOccurrenceFact placement{};
                // One identity can hold two transforms in a scenario, so the scenario-local
                // ordinal is what keeps the id unique.
                if (!format_text(placement.id,
                                 "external-placement/%08x/%08x/%016llx",
                                 static_cast<unsigned>(scenarioTag),
                                 static_cast<unsigned>(rowOrdinal),
                                 static_cast<unsigned long long>(row.placedEntryIdentity))) {
                    return false;
                }
                placement.external = true;
                placement.scenarioIndex = scenarioIndex;
                placement.objectListTag = row.objectListTag;
                placement.placementOrdinal = row.placementOrdinal;
                placement.placedEntryIdentity = row.placedEntryIdentity;
                placement.positionBits = row.positionBits;
                placement.quaternionBits = row.quaternionBits;
                placement.uniformScaleBits = row.uniformScaleBits;
                placement.nameHash = row.nameHash;
                placement.actorDefinitionTag = row.classListTag;
                placement.declaredBubbleIndex = tables::kGlobalBubbleIndex;
                placement.complete = true;
                facts.placementOccurrences.push_back(std::move(placement));
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
