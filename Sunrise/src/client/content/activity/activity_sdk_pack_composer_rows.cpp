#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "activity_sdk_activity_inventory.h"
#include "activity_sdk_actor_rsat_inventory.h"
#include "activity_sdk_authored_scene_inventory.h"
#include "activity_sdk_behavior_inventory.h"
#include "activity_sdk_pack_composer_internal.h"
#include "activity_sdk_policy_inventory.h"
#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_topology_enrichment.h"
#include "activity_sdk_topology_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::pack_composer {
namespace {

/** Format-v9 state rows use bits zero and one for enabled and extraction-complete state. */
constexpr std::uint32_t kStateEnabled = 0x1U;
constexpr std::uint32_t kStateExtractionComplete = 0x2U;
static_assert((kStateEnabled | kStateExtractionComplete) == format::kStateFlagMask);

template <typename Text>
[[nodiscard]] bool text_view(const Text& input, std::string_view& output) noexcept {
    output = {};
    if (input.length >= input.value.size() || input.value[input.length] != '\0') {
        return false;
    }
    output = {input.value.data(), input.length};
    return true;
}

[[nodiscard]] bool policy_text_view(const policy_inventory::Snapshot& source,
                                    policy_inventory::Text input,
                                    std::string_view& output) noexcept {
    output = {};
    if (input.stringIndex >= source.strings.size()) {
        return false;
    }
    output = source.strings[input.stringIndex];
    return true;
}

template <typename Text>
[[nodiscard]] bool link_text(const detail::StringResolver& strings,
                             const Text& input,
                             format::StringRef& output) noexcept {
    std::string_view value{};
    return text_view(input, value) && strings.resolve(value, output);
}

[[nodiscard]] bool link_string(const detail::StringResolver& strings,
                               std::string_view input,
                               format::StringRef& output) noexcept {
    return strings.resolve(input, output);
}

[[nodiscard]] bool link_policy_text(const detail::StringResolver& strings,
                                    const policy_inventory::Snapshot& source,
                                    policy_inventory::Text input,
                                    format::StringRef& output) noexcept {
    std::string_view value{};
    return policy_text_view(source, input, value) && strings.resolve(value, output);
}

[[nodiscard]] int hexadecimal_digit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

/** Decodes the generated lowercase SHA-256 spelling without accepting a partial digest. */
[[nodiscard]] bool decode_digest(std::string_view input,
                                 std::array<std::byte, 32>& output) noexcept {
    output = {};
    if (input.size() != output.size() * 2U) {
        return false;
    }
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = hexadecimal_digit(input[index * 2U]);
        const int low = hexadecimal_digit(input[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            output = {};
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

template <typename Enum> [[nodiscard]] std::uint32_t semantic_ordinal(Enum value) noexcept {
    return static_cast<std::uint32_t>(value) + 1U;
}

/** Appends one canonical tag set and returns its exact range in the shared tag section. */
[[nodiscard]] bool append_binding_tags(std::span<const std::uint32_t> source,
                                       std::vector<format::ActivityBindingTag>& rows,
                                       format::Range& output) {
    if (rows.size() >= format::kAbsentIndex || source.size() > format::kAbsentIndex - rows.size()) {
        return false;
    }
    output = {static_cast<std::uint32_t>(rows.size()), static_cast<std::uint32_t>(source.size())};
    for (const std::uint32_t tag : source) {
        rows.push_back({tag});
    }
    return true;
}

/** Appends exact package locators and returns their activity-owned range. */
[[nodiscard]] bool
append_binding_locators(std::span<const activity_inventory::PackageLocator> source,
                        std::vector<format::ActivityBindingLocator>& rows,
                        format::Range& output) {
    if (rows.size() >= format::kAbsentIndex || source.size() > format::kAbsentIndex - rows.size()) {
        return false;
    }
    output = {static_cast<std::uint32_t>(rows.size()), static_cast<std::uint32_t>(source.size())};
    for (const activity_inventory::PackageLocator& locator : source) {
        rows.push_back({locator.tag, 0, locator.offset});
    }
    return true;
}

/**
 * Translates activity topology and its direct child sections.
 * @param inputs Accepted native inventories.
 * @param ranges Final parent-to-child ranges.
 * @param linker Resolver for the frozen string table.
 * @param output Composer-owned final rows.
 * @return True when every source row and string reference is valid.
 */
[[nodiscard]] bool translate_topology(const Inputs& inputs,
                                      const detail::PreparedRanges& ranges,
                                      const detail::StringResolver& linker,
                                      Storage& output) {
    const topology_inventory::Snapshot& topology = *inputs.topology;
    const topology_enrichment::Snapshot& enrichment = *inputs.topologyEnrichment;
    const policy_inventory::Snapshot& policy = *inputs.policy;
    const activity_inventory::Snapshot& activityInventory = *inputs.activityInventory;

    output.activities.resize(topology.activities.size());
    for (std::size_t index = 0; index < topology.activities.size(); ++index) {
        const topology_inventory::Activity& source = topology.activities[index];
        const activity_inventory::ActivityVariant& binding = activityInventory.activities[index];
        format::Activity& target = output.activities[index];
        target.activityIndex = source.activityIndex;
        target.definitionHash = source.definitionHash;
        target.scenarioIndex = source.scenarioIndex;
        target.flags = source.exactFlags;
        target.aliases = policy.activityAliases[index];
        target.capabilities = ranges.activityCapabilities[index];
        target.selectedActivityRootTag =
            binding.activityRootTag == 0 ? format::kAbsentIndex : binding.activityRootTag;
        target.selectedScenarioTag =
            binding.scenarioTag == 0 ? format::kAbsentIndex : binding.scenarioTag;
        target.matchmakingConfigTag = binding.bindingEvidence.matchmakingConfigTag;
        target.joinStatus = static_cast<std::uint32_t>(binding.joinStatus);
        target.bindingDisposition = static_cast<std::uint32_t>(binding.bindingDisposition);
        target.bindingReason = static_cast<std::uint32_t>(binding.bindingReason);
        target.bindingEvidenceBasis = static_cast<std::uint32_t>(binding.bindingEvidenceBasis);
        target.runnableStatus = static_cast<std::uint32_t>(binding.runnableStatus);
        target.bindingFlags =
            (binding.fullSdkAcceptable ? format::kActivityBindingFullSdkAcceptable : 0U)
            | (binding.bindingEvidence.hasInternalName ? format::kActivityBindingHasInternalName
                                                       : 0U)
            | (binding.bindingEvidence.hasMatchmakingConfig
                   ? format::kActivityBindingHasMatchmakingConfig
                   : 0U);
        if (!link_text(linker, source.id, target.id)
            || !link_text(linker, source.internalName, target.internalName)
            || !link_text(linker, source.displayName, target.displayName)
            || !append_binding_tags(binding.bindingEvidence.activityRootCandidateTags,
                                    output.activityBindingTags,
                                    target.activityRootCandidateTags)
            || !append_binding_tags(binding.bindingEvidence.scenarioNameCandidateTags,
                                    output.activityBindingTags,
                                    target.scenarioNameCandidateTags)
            || !append_binding_tags(binding.bindingEvidence.evidenceRootTags,
                                    output.activityBindingTags,
                                    target.evidenceRootTags)
            || !append_binding_locators(binding.bindingEvidence.locators,
                                        output.activityBindingLocators,
                                        target.bindingLocators)) {
            return false;
        }
    }

    output.scenarios.resize(topology.scenarios.size());
    for (std::size_t index = 0; index < topology.scenarios.size(); ++index) {
        const topology_inventory::Scenario& source = topology.scenarios[index];
        format::Scenario& target = output.scenarios[index];
        target.tag = source.tag;
        target.bubbles = ranges.scenarioBubbles[index];
        target.states = ranges.scenarioStates[index];
        target.occurrences = ranges.scenarioOccurrences[index];
        if (!link_text(linker, source.id, target.id)
            || !link_text(linker, source.name, target.name)) {
            return false;
        }
    }

    output.bubbles.resize(topology.bubbles.size());
    for (std::size_t index = 0; index < topology.bubbles.size(); ++index) {
        const topology_inventory::Bubble& source = topology.bubbles[index];
        format::Bubble& target = output.bubbles[index];
        target.scenarioIndex = source.scenarioIndex;
        target.bubbleOrdinal = source.bubbleOrdinal;
        target.nameHash = source.nameHash;
        target.states = ranges.bubbleStates[index];
        if (!link_text(linker, source.id, target.id)
            || !link_text(linker, enrichment.bubbleNames[index], target.name)) {
            return false;
        }
    }

    output.states.resize(topology.states.size());
    for (std::size_t index = 0; index < topology.states.size(); ++index) {
        const topology_inventory::State& source = topology.states[index];
        format::State& target = output.states[index];
        target.scenarioIndex = source.scenarioIndex;
        target.bubbleIndex = source.bubbleIndex;
        target.stateOrdinal = source.stateOrdinal;
        target.entryIndex = source.entryIndex;
        target.sliceSetIndex = source.sliceSetIndex;
        target.mapBubbleIndex = source.mapBubbleIndex;
        target.stateHash = source.stateHash;
        target.publicValue = source.rawU32At12;
        target.flags = kStateExtractionComplete | (source.enabled ? kStateEnabled : 0U);
        target.registryTag = source.registryTag;
        if (!link_text(linker, source.id, target.id)
            || !link_text(linker, source.entryId, target.entryId)
            || !link_text(linker, source.registryId, target.registryId)) {
            return false;
        }
    }

    output.objects.resize(topology.objects.size());
    for (std::size_t index = 0; index < topology.objects.size(); ++index) {
        const topology_inventory::Object& source = topology.objects[index];
        format::Object& target = output.objects[index];
        target.objectTag = source.objectTag;
        target.objectKey = source.objectKey;
        target.slots = ranges.objectSlots[index];
        target.configCount = source.configCount;
        target.descriptorCount = source.descriptorCount;
        target.placedSubblockCount = source.placedSubblockCount;
        target.placedLeafCount = source.placedLeafCount;
        target.placedHopCount = source.placedHopCount;
        target.bareTargetCount = source.bareTargetCount;
        target.replicatedPlacementCount = source.replicatedPlacementCount;
        if (!link_text(linker, source.id, target.id)) {
            return false;
        }
    }

    output.occurrences.resize(topology.occurrences.size());
    for (std::size_t index = 0; index < topology.occurrences.size(); ++index) {
        const topology_inventory::Occurrence& source = topology.occurrences[index];
        format::Occurrence& target = output.occurrences[index];
        target.scenarioIndex = source.scenarioIndex;
        target.bubbleIndex = source.bubbleIndex;
        target.stateIndex = source.stateIndex;
        target.objectIndex = source.objectIndex;
        target.registryField = source.registryField;
        target.objectOrdinal = source.objectOrdinal;
        if (!link_text(linker, source.id, target.id)
            || !link_text(linker, source.contextRegistryKey, target.contextRegistryKey)
            || !link_text(linker, source.registryId, target.registryId)
            || !link_text(linker, source.entryId, target.entryId)) {
            return false;
        }
    }

    output.slots.resize(topology.slots.size());
    for (std::size_t index = 0; index < topology.slots.size(); ++index) {
        const topology_inventory::Slot& source = topology.slots[index];
        const topology_enrichment::Slot& linked = enrichment.slots[index];
        format::Slot& target = output.slots[index];
        target.objectIndex = source.objectIndex;
        target.slotIndex = source.slotIndex;
        target.slotType = source.slotType;
        target.componentClass = linked.componentClass;
        target.senseSchema = linked.senseSchema;
        target.authSchema = linked.authSchema;
        target.flags = linked.flags;
        target.reserved = linked.dialogueCueCount;
        target.aliases = policy.slotAliases[index];
        target.capabilities = ranges.slotCapabilities[index];
        if (!link_text(linker, source.id, target.id) || !link_text(linker, linked.name, target.name)
            || !link_text(linker, linked.senseSchemaId, target.senseSchemaId)
            || !link_text(linker, linked.authSchemaId, target.authSchemaId)) {
            return false;
        }
    }
    return true;
}

/** Copies policy rows after resolving every handle in the policy-owned pool. */
[[nodiscard]] bool translate_policy(const policy_inventory::Snapshot& source,
                                    const detail::StringResolver& linker,
                                    Storage& output) {
    output.texts.resize(source.texts.size());
    for (std::size_t index = 0; index < source.texts.size(); ++index) {
        const policy_inventory::TextRow& input = source.texts[index];
        format::Text& target = output.texts[index];
        target.kind = input.kind;
        target.reserved = input.reserved;
        if (!link_policy_text(linker, source, input.value, target.value)) {
            return false;
        }
    }

    output.capabilities.resize(source.capabilities.size());
    for (std::size_t index = 0; index < source.capabilities.size(); ++index) {
        const policy_inventory::Capability& input = source.capabilities[index];
        format::Capability& target = output.capabilities[index];
        target.subjectKind = input.subjectKind;
        target.subjectIndex = input.subjectIndex;
        target.exposureFlags = input.exposureFlags;
        target.candidateExposureFlags = input.candidateExposureFlags;
        target.gates = input.gates;
        target.refusals = input.refusals;
        if (!link_policy_text(linker, source, input.id, target.id)
            || !link_policy_text(linker, source, input.operation, target.operation)
            || !link_policy_text(linker, source, input.valueSchemaId, target.valueSchemaId)) {
            return false;
        }
    }

    output.gates.resize(source.gates.size());
    for (std::size_t index = 0; index < source.gates.size(); ++index) {
        const policy_inventory::Gate& input = source.gates[index];
        format::Gate& target = output.gates[index];
        if (!link_policy_text(linker, source, input.gate, target.gate)
            || !link_policy_text(linker, source, input.status, target.status)
            || !link_policy_text(linker, source, input.reasonCode, target.reasonCode)
            || !link_policy_text(linker, source, input.required, target.required)
            || !link_policy_text(linker, source, input.observed, target.observed)
            || !link_policy_text(linker, source, input.wouldConfirm, target.wouldConfirm)) {
            return false;
        }
    }

    output.refusals.resize(source.refusals.size());
    for (std::size_t index = 0; index < source.refusals.size(); ++index) {
        const policy_inventory::Refusal& input = source.refusals[index];
        format::Refusal& target = output.refusals[index];
        target.reasonCodes = input.reasonCodes;
        target.capabilityIndex = input.capabilityIndex;
        target.reserved = input.reserved;
        if (!link_policy_text(linker, source, input.id, target.id)
            || !link_policy_text(linker, source, input.exposure, target.exposure)
            || !link_policy_text(linker, source, input.status, target.status)) {
            return false;
        }
    }
    return true;
}

/** Copies actor, RSAT, squad, and panel placement sections. */
[[nodiscard]] bool
translate_native_rows(const Inputs& inputs, const detail::StringResolver& linker, Storage& output) {
    const actor_rsat_inventory::Snapshot& actorRsat = *inputs.actorRsat;
    const squad_inventory::Snapshot& squads = *inputs.squads;
    const authored_scene_inventory::Snapshot& authoredScenes = *inputs.authoredScenes;
    const behavior_inventory::Snapshot& behaviors = *inputs.behaviors;

    output.actorClasses.resize(actorRsat.actorClasses.size());
    for (std::size_t index = 0; index < actorRsat.actorClasses.size(); ++index) {
        const actor_rsat_inventory::ActorClass& input = actorRsat.actorClasses[index];
        format::ActorClass& target = output.actorClasses[index];
        target.definitionTag = input.definitionTag;
        target.nameHash = input.nameHash;
        target.rsatTag = input.rsatTag;
        target.rsatReverseDefinitionTag = input.rsatReverseDefinitionTag;
        target.objectType = input.objectType;
        target.descriptorArrayOffset = input.descriptorArrayOffset;
        target.descriptorArrayRelative = input.descriptorArrayRelative;
        target.descriptorArrayHeaderOffset = input.descriptorArrayHeaderOffset;
        target.descriptorArrayDataOffset = input.descriptorArrayDataOffset;
        target.descriptorElementClass = input.descriptorElementClass;
        target.descriptors = input.descriptors;
        target.dynamicPresenceTailCount = input.dynamicPresenceTailCount;
        target.authoredSpawnProfile = input.authoredSpawnProfile;
        if (!link_text(linker, input.id, target.id)) {
            return false;
        }
    }

    output.actorMessageSchemas.resize(actorRsat.messageSchemas.size());
    for (std::size_t index = 0; index < actorRsat.messageSchemas.size(); ++index) {
        const actor_rsat_inventory::ActorMessageSchema& input = actorRsat.messageSchemas[index];
        format::ActorMessageSchema& target = output.actorMessageSchemas[index];
        target.definitionHandle = input.definitionHandle;
        target.durableKey = input.durableKey;
        target.ownerClass = input.ownerClass;
        target.handlerSlot = input.handlerSlot;
        target.bodyType = input.bodyType;
        target.provenance = input.provenance;
        target.commands = input.commands;
        target.flags = input.flags;
        if (!link_text(linker, input.name, target.name)) {
            return false;
        }
    }

    output.actorCommandDefinitions.resize(actorRsat.commandDefinitions.size());
    for (std::size_t index = 0; index < actorRsat.commandDefinitions.size(); ++index) {
        const actor_rsat_inventory::ActorCommandDefinition& input =
            actorRsat.commandDefinitions[index];
        format::ActorCommandDefinition& target = output.actorCommandDefinitions[index];
        target.selector = input.selector;
        target.payloadHandle = input.payloadHandle;
        target.effect = input.effect;
        target.provenance = input.provenance;
        target.factionNone = input.factionNone;
        target.factionRemoved = input.factionRemoved;
        target.factionHostileToAll = input.factionHostileToAll;
        target.flags = input.flags;
        if (!link_text(linker, input.name, target.name)
            || !link_text(linker, input.factionNoneName, target.factionNoneName)
            || !link_text(linker, input.factionRemovedName, target.factionRemovedName)
            || !link_text(linker, input.factionHostileToAllName, target.factionHostileToAllName)) {
            return false;
        }
    }

    output.actorBehaviorProfiles.resize(actorRsat.behaviorProfiles.size());
    for (std::size_t index = 0; index < actorRsat.behaviorProfiles.size(); ++index) {
        const actor_rsat_inventory::ActorBehaviorProfile& input = actorRsat.behaviorProfiles[index];
        format::ActorBehaviorProfile& target = output.actorBehaviorProfiles[index];
        target.actorClassIndex = input.actorClassIndex;
        target.behaviorConfigTag = input.behaviorConfigTag;
        target.behaviorConfigClass = input.behaviorConfigClass;
        target.behaviorConfigOffset = input.behaviorConfigOffset;
        target.defaultFaction = input.defaultFaction;
        target.behaviorProvenance = input.behaviorProvenance;
        target.factionProvenance = input.factionProvenance;
        target.flags = input.flags;
    }

    output.simulationEventDefinitions.resize(actorRsat.simulationEvents.size());
    for (std::size_t index = 0; index < actorRsat.simulationEvents.size(); ++index) {
        const actor_rsat_inventory::SimulationEventDefinition& input =
            actorRsat.simulationEvents[index];
        format::SimulationEventDefinition& target = output.simulationEventDefinitions[index];
        target.eventType = input.eventType;
        target.primarySchema = input.primarySchema;
        target.secondarySchema = input.secondarySchema;
        target.provenance = input.provenance;
        target.descriptorEvidenceAddress = input.descriptorEvidenceAddress;
        target.primaryEvidenceAddress = input.primaryEvidenceAddress;
        target.secondaryEvidenceAddress = input.secondaryEvidenceAddress;
        target.flags = input.flags;
        if (!link_text(linker, input.name, target.name)) {
            return false;
        }
    }
    output.runtimeSchemas = actorRsat.runtimeSchemas;
    output.runtimeFields = actorRsat.runtimeFields;
    output.runtimeTypeDefinitions.resize(actorRsat.runtimeTypes.size());
    for (std::size_t index = 0; index < actorRsat.runtimeTypes.size(); ++index) {
        const actor_rsat_inventory::RuntimeTypeDefinition& input = actorRsat.runtimeTypes[index];
        format::RuntimeTypeDefinition& target = output.runtimeTypeDefinitions[index];
        target.codecFamilies = input.codecFamilies;
        target.typeCode = input.typeCode;
        target.decodedSize = input.decodedSize;
        target.fixedBits = input.fixedBits;
        target.minimumBits = input.minimumBits;
        target.maximumBits = input.maximumBits;
        target.writerEvidenceAddress = input.writerEvidenceAddress;
        target.readerEvidenceAddress = input.readerEvidenceAddress;
        target.flags = input.flags;
        if (!link_text(linker, input.name, target.name)) {
            return false;
        }
    }
    output.sobjectRsats = actorRsat.sobjectRsats;
    output.sobjectRsatDescriptors = actorRsat.sobjectRsatDescriptors;
    output.sobjectRsatFieldBindings = actorRsat.sobjectRsatFieldBindings;
    output.actorStateNames = actorRsat.actorStateNames;
    output.actorSequenceTables = actorRsat.sequenceTables;
    output.actorSequenceBindings = actorRsat.sequenceBindings;
    output.actorSequenceEntries.reserve(actorRsat.sequenceEntries.size());
    for (const auto& input : actorRsat.sequenceEntries) {
        auto row = input.row;
        if (!link_string(linker, input.id, row.id) || !link_string(linker, input.name, row.name)
            || !link_string(linker, input.symbol, row.symbol)
            || !link_string(linker, input.sourcePath, row.sourcePath)) {
            return false;
        }
        output.actorSequenceEntries.push_back(row);
    }
    output.entityTypeDefinitions.resize(actorRsat.entityTypes.size());
    for (std::size_t index = 0; index < actorRsat.entityTypes.size(); ++index) {
        const actor_rsat_inventory::EntityTypeDefinition& input = actorRsat.entityTypes[index];
        format::EntityTypeDefinition& target = output.entityTypeDefinitions[index];
        target.entityType = input.entityType;
        target.baselineSchema = input.baselineSchema;
        target.updateSchema = input.updateSchema;
        target.provenance = input.provenance;
        target.vtableEvidenceAddress = input.vtableEvidenceAddress;
        target.baselineEvidenceAddress = input.baselineEvidenceAddress;
        target.updateEvidenceAddress = input.updateEvidenceAddress;
        target.flags = input.flags;
        if (!link_text(linker, input.name, target.name)) {
            return false;
        }
    }

    output.rsatDescriptors.resize(actorRsat.descriptors.size());
    for (std::size_t index = 0; index < actorRsat.descriptors.size(); ++index) {
        const actor_rsat_inventory::RsatDescriptor& input = actorRsat.descriptors[index];
        format::RsatDescriptor& target = output.rsatDescriptors[index];
        target.actorClassIndex = input.actorClassIndex;
        target.rsatTag = input.rsatTag;
        target.descriptorOrdinal = input.descriptorOrdinal;
        target.descriptorOffset = input.descriptorOffset;
        target.descriptorElementClass = input.descriptorElementClass;
        target.componentTag = input.componentTag;
        target.schemaIndex = input.schemaIndex;
        target.schemaTag = input.schemaTag;
        target.schemaFieldCount = input.schemaFieldCount;
        target.schemaFirstFieldRuntimeGate = input.schemaFirstFieldRuntimeGate;
        target.schemaFirstFieldRawU32At10 = input.schemaFirstFieldRawU32At10;
        target.flags = input.flags;
        target.dynamicPresenceTailOrdinal = input.dynamicPresenceTailOrdinal;
        target.rawRow = input.rawRow;
        if (!link_text(linker, input.id, target.id)) {
            return false;
        }
    }

    output.rsatSchemas.resize(actorRsat.schemas.size());
    for (std::size_t index = 0; index < actorRsat.schemas.size(); ++index) {
        const actor_rsat_inventory::RsatSchema& input = actorRsat.schemas[index];
        format::RsatSchema& target = output.rsatSchemas[index];
        target.schemaTag = input.schemaTag;
        target.schemaClass = input.schemaClass;
        target.fieldCount = input.fieldCount;
        target.fieldArrayOffset = input.fieldArrayOffset;
        target.fieldArrayRelative = input.fieldArrayRelative;
        target.fieldArrayHeaderOffset = input.fieldArrayHeaderOffset;
        target.fieldArrayDataOffset = input.fieldArrayDataOffset;
        target.fieldElementClass = input.fieldElementClass;
        target.firstFieldRuntimeGate = input.firstFieldRuntimeGate;
        target.firstFieldRawU32At10 = input.firstFieldRawU32At10;
        target.flags = input.flags;
        target.fields = input.fields;
        if (!link_text(linker, input.id, target.id)) {
            return false;
        }
    }
    output.rsatFields.resize(actorRsat.fields.size());
    for (std::size_t index = 0; index < actorRsat.fields.size(); ++index) {
        output.rsatFields[index].rawRow = actorRsat.fields[index].rawRow;
    }

    output.squads.resize(squads.squads.size());
    for (std::size_t index = 0; index < squads.squads.size(); ++index) {
        const squad_inventory::Squad& input = squads.squads[index];
        format::Squad& target = output.squads[index];
        target.scenarioIndex = input.scenarioIndex;
        target.objectIndex = input.objectIndex;
        target.slotIndex = input.slotIndex;
        target.spawnerConfigTag = input.spawnerConfigTag;
        target.spawnRuleConfigTag = input.spawnRuleConfigTag;
        target.flags = input.flags;
        target.occurrenceIndex = input.occurrenceIndex;
        target.members = input.members;
        target.anchors = input.anchors;
        if (!link_string(linker, input.id, target.id)) {
            return false;
        }
    }

    output.squadMembers.resize(squads.members.size());
    for (std::size_t index = 0; index < squads.members.size(); ++index) {
        const squad_inventory::SquadMember& input = squads.members[index];
        format::SquadMember& target = output.squadMembers[index];
        target.squadIndex = input.squadIndex;
        target.memberOrdinal = input.memberOrdinal;
        target.memberKey = input.memberKey;
        target.actorClassIndex = input.actorClassIndex;
        target.flags = input.flags;
        target.candidateCounts = input.candidateCounts;
        target.defaultCount = input.defaultCount;
        if (!link_string(linker, input.id, target.id)) {
            return false;
        }
    }

    output.squadAnchors.resize(squads.anchors.size());
    for (std::size_t index = 0; index < squads.anchors.size(); ++index) {
        const squad_inventory::SquadAnchor& input = squads.anchors[index];
        format::SquadAnchor& target = output.squadAnchors[index];
        target.squadIndex = input.squadIndex;
        target.pointOrdinal = input.pointOrdinal;
        target.objectListTag = input.objectListTag;
        target.placementOrdinal = input.placementOrdinal;
        target.flags = input.flags;
        target.placedEntryIdentity = input.placedEntryIdentity;
        target.positionBits = input.positionBits;
        if (!link_string(linker, input.id, target.id)) {
            return false;
        }
    }

    output.authoredSceneResources.resize(authoredScenes.resources.size());
    for (std::size_t index = 0; index < authoredScenes.resources.size(); ++index) {
        const authored_scene_inventory::Resource& input = authoredScenes.resources[index];
        format::AuthoredSceneResource& target = output.authoredSceneResources[index];
        target.slotIndex = input.slotIndex;
        target.configTag = input.configTag;
        target.descriptorOffset = input.descriptorOffset;
        target.resourceFieldOffset = input.resourceFieldOffset;
        target.resourceTag = input.resourceTag;
        target.resourceClass = input.resourceClass;
        target.flags = input.flags;
        target.reserved = input.reserved;
        if (!link_text(linker, input.id, target.id)) {
            return false;
        }
    }

    output.authoredSceneSquadEdges.resize(authoredScenes.squadEdges.size());
    for (std::size_t index = 0; index < authoredScenes.squadEdges.size(); ++index) {
        const authored_scene_inventory::SquadEdge& input = authoredScenes.squadEdges[index];
        format::AuthoredSceneSquadEdge& target = output.authoredSceneSquadEdges[index];
        target.sceneSlotIndex = input.sceneSlotIndex;
        target.squadSlotIndex = input.squadSlotIndex;
        target.configTag = input.configTag;
        target.descriptorOffset = input.descriptorOffset;
        target.referenceFieldOffset = input.referenceFieldOffset;
        target.targetObjectKey = input.targetObjectKey;
        target.flags = input.flags;
        target.reserved = input.reserved;
        if (!link_text(linker, input.id, target.id)) {
            return false;
        }
    }
    output.taskTargets.resize(authoredScenes.taskTargets.size());
    for (std::size_t index = 0; index < authoredScenes.taskTargets.size(); ++index) {
        const authored_scene_inventory::TaskTarget& input = authoredScenes.taskTargets[index];
        format::TaskTarget& target = output.taskTargets[index];
        target.taskSlotIndex = input.taskSlotIndex;
        target.objectiveSlotIndex = input.objectiveSlotIndex;
        target.configTag = input.configTag;
        target.descriptorOffset = input.descriptorOffset;
        target.referenceFieldOffset = input.referenceFieldOffset;
        target.targetObjectKey = input.targetObjectKey;
        target.bitIndex = input.bitIndex;
        target.flags = input.flags;
        target.reserved = input.reserved;
        if (!link_text(linker, input.id, target.id)) {
            return false;
        }
    }
    output.dialogueCueTexts.resize(authoredScenes.dialogueCueTexts.size());
    for (std::size_t index = 0; index < authoredScenes.dialogueCueTexts.size(); ++index) {
        const authored_scene_inventory::DialogueCueText& input =
            authoredScenes.dialogueCueTexts[index];
        format::DialogueCueText& target = output.dialogueCueTexts[index];
        target.slotIndex = input.slotIndex;
        target.cueIndex = input.cueIndex;
        target.definitionHash = input.definitionHash;
        target.containerTag = input.containerTag;
        target.stringHash = input.stringHash;
        if (!link_text(linker, input.id, target.id)
            || !link_text(linker, input.text, target.text)) {
            return false;
        }
    }
    output.directiveElements.resize(authoredScenes.directiveElements.size());
    for (std::size_t index = 0; index < authoredScenes.directiveElements.size(); ++index) {
        const authored_scene_inventory::DirectiveElement& input =
            authoredScenes.directiveElements[index];
        format::DirectiveElement& target = output.directiveElements[index];
        target.slotIndex = input.slotIndex;
        target.nameHash = input.nameHash;
        target.elementIndex = input.elementIndex;
        target.elementCount = input.elementCount;
        target.titleContainerTag = input.titleContainerTag;
        target.titleStringHash = input.titleStringHash;
        target.descriptionContainerTag = input.descriptionContainerTag;
        target.descriptionStringHash = input.descriptionStringHash;
        if (!link_text(linker, input.id, target.id) || !link_text(linker, input.title, target.title)
            || !link_text(linker, input.description, target.description)) {
            return false;
        }
    }

    output.behaviorPrograms.reserve(behaviors.programs.size());
    for (const behavior_inventory::Program& input : behaviors.programs) {
        output.behaviorPrograms.push_back({input.rootTag,
                                           {input.firstInput, input.inputCount},
                                           {input.firstWrite, input.writeCount},
                                           input.nodeCount,
                                           input.expressionCount});
    }
    output.behaviorInputs.reserve(behaviors.inputs.size());
    for (const behavior_inventory::Input& input : behaviors.inputs) {
        output.behaviorInputs.push_back({input.programRow,
                                         input.nodeOffset,
                                         input.expressionOffset,
                                         input.channelHash,
                                         input.inputOrMode,
                                         input.nativeOverride,
                                         input.activeField,
                                         input.selector,
                                         input.role,
                                         0});
    }
    output.behaviorChannelWrites.reserve(behaviors.writes.size());
    for (const behavior_inventory::ChannelWrite& input : behaviors.writes) {
        output.behaviorChannelWrites.push_back(
            {input.programRow, input.nodeOffset, input.channelHash, 0});
    }
    output.behaviorOwners.reserve(behaviors.owners.size());
    for (const behavior_inventory::Owner& input : behaviors.owners) {
        output.behaviorOwners.push_back(
            {input.programRow,
             input.actorClassIndex,
             input.configTag,
             input.configFieldOffset,
             input.buildOrdinal,
             input.descriptorOrdinal,
             input.submitterSubtype,
             static_cast<format::BehaviorSubmissionKind>(input.submissionKind)});
    }
    for (std::size_t ownerIndex = 0; ownerIndex < output.behaviorOwners.size(); ++ownerIndex) {
        if (ownerIndex > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        const format::BehaviorOwner& owner = output.behaviorOwners[ownerIndex];
        for (std::size_t memberIndex = 0; memberIndex < output.squadMembers.size(); ++memberIndex) {
            if (memberIndex > (std::numeric_limits<std::uint32_t>::max)()) {
                return false;
            }
            const format::SquadMember& member = output.squadMembers[memberIndex];
            if (member.actorClassIndex != owner.actorClassIndex
                || member.squadIndex >= output.squads.size()) {
                continue;
            }
            const format::Squad& squad = output.squads[member.squadIndex];
            if (squad.occurrenceIndex >= output.occurrences.size()) {
                return false;
            }
            const format::Occurrence& occurrence = output.occurrences[squad.occurrenceIndex];
            output.behaviorActivityBindings.push_back({static_cast<std::uint32_t>(ownerIndex),
                                                       member.squadIndex,
                                                       static_cast<std::uint32_t>(memberIndex),
                                                       occurrence.scenarioIndex,
                                                       squad.occurrenceIndex,
                                                       occurrence.stateIndex,
                                                       occurrence.objectIndex,
                                                       0});
        }
    }
    return true;
}

} // namespace

/** Translates topology, policy and native rows into pack storage. @return False on failure. */
bool detail::translate_rows(const Inputs& inputs,
                            const PreparedRanges& ranges,
                            const StringResolver& strings,
                            Storage& output) {
    if (!translate_topology(inputs, ranges, strings, output)) {
        return false;
    }
    if (!translate_policy(*inputs.policy, strings, output)) {
        return false;
    }
    if (!translate_native_rows(inputs, strings, output)) {
        return false;
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::pack_composer
