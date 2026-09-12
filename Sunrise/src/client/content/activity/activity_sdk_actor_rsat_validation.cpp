// Owns the pinned engine-semantics table, the copy that installs it, and the snapshot validator.
// The table is large, so exactly one translation unit may include it.
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "activity_sdk_actor_rsat_inventory_internal.h"
#include "activity_sdk_actor_sequences.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
namespace {

#include "activity_sdk_actor_engine_semantics.inc"

/** Copies one bounded catalog name into fixed inventory storage. */
[[nodiscard]] bool text_literal(std::string_view value, Text& output) noexcept {
    output = {};
    if (value.empty() || value.size() >= output.value.size()) {
        return false;
    }
    std::copy(value.begin(), value.end(), output.value.begin());
    output.length = static_cast<std::uint16_t>(value.size());
    return true;
}

/** Accepts only one bounded string followed by zero-filled storage. */
[[nodiscard]] bool valid_text(const Text& text) noexcept {
    if (text.length >= text.value.size() || text.value[text.length] != '\0') {
        return false;
    }
    for (std::size_t index = 0; index < text.length; ++index) {
        if (text.value[index] == '\0') {
            return false;
        }
    }
    return std::all_of(text.value.begin() + text.length + 1U, text.value.end(), [](char value) {
        return value == '\0';
    });
}

[[nodiscard]] bool same_text(const Text& left, const Text& right) noexcept {
    return valid_text(left) && valid_text(right) && left.length == right.length
           && left.value == right.value;
}

[[nodiscard]] bool range_inside(format::Range range, std::size_t size) noexcept {
    return range.first <= size && range.count <= size - range.first;
}

/** Field-5 lanes 1..4 are bias-one values with widths 2, 3, 2, and 3. */
[[nodiscard]] bool valid_authored_spawn_profile(const ActorClass& actor) noexcept {
    // Logical maxima of the four lanes after the bias-one decode.
    constexpr std::array<std::int8_t, 4> kMaximumLogical{2, 6, 2, 6};
    for (std::size_t index = 0; index < actor.authoredSpawnProfile.size(); ++index) {
        if (actor.authoredSpawnProfile[index] < 0
            || actor.authoredSpawnProfile[index] > kMaximumLogical[index]) {
            return false;
        }
    }
    return true;
}

/** Checks an exact stored self-relative header and data pair. */
[[nodiscard]] bool stored_typed_array(std::uint32_t field,
                                      std::int64_t relative,
                                      std::uint32_t header,
                                      std::uint32_t data) noexcept {
    std::size_t expected = 0;
    return relative != 0
           && relative_offset(static_cast<std::size_t>(field) + 8U, relative, expected)
           && expected <= (std::numeric_limits<std::uint32_t>::max)()
           && header == static_cast<std::uint32_t>(expected)
           && header <= (std::numeric_limits<std::uint32_t>::max)() - 16U && data == header + 16U;
}

} // namespace

/** Adds the pinned actor command semantics shared by every installed actor. */
bool add_engine_semantics(Snapshot& snapshot) {
    const ExtractedActorEngineSemantics& extracted = kExtractedActorEngineSemantics;
    const bool hasIdentity = std::any_of(extracted.executableIdentity.begin(),
                                         extracted.executableIdentity.end(),
                                         [](std::byte value) { return value != std::byte{}; });
    if (extracted.version == 0 || !hasIdentity || extracted.message.evidenceAddress == 0) {
        return false;
    }
    ActorMessageSchema message{};
    if (!text_literal(extracted.message.name, message.name)) {
        return false;
    }
    message.definitionHandle = extracted.message.definitionHandle;
    message.durableKey = extracted.message.durableKey;
    message.ownerClass = extracted.message.ownerClass;
    message.handlerSlot = extracted.message.handlerSlot;
    message.bodyType = extracted.message.bodyType;
    message.commands = {0, static_cast<std::uint32_t>(extracted.commands.size())};
    message.flags = format::kActorMessageSchemaExact;
    snapshot.messageSchemas.push_back(message);
    for (const ExtractedActorCommandDefinition& input : extracted.commands) {
        ActorCommandDefinition command{};
        if (input.metadataEvidenceAddress == 0 || !text_literal(input.name, command.name)) {
            return false;
        }
        command.selector = input.selector;
        command.payloadHandle = input.payloadHandle;
        command.effect = input.setFaction ? format::ActorCommandEffect::setFaction
                                          : format::ActorCommandEffect::opaque;
        if (input.setFaction) {
            if (input.effectEvidenceAddress == 0
                || !text_literal(input.factionNoneName, command.factionNoneName)
                || !text_literal(input.factionRemovedName, command.factionRemovedName)
                || !text_literal(input.factionHostileToAllName, command.factionHostileToAllName)) {
                return false;
            }
            command.factionNone = input.factionNone;
            command.factionRemoved = input.factionRemoved;
            command.factionHostileToAll = input.factionHostileToAll;
        }
        command.flags = format::kActorCommandDefinitionExact;
        snapshot.commandDefinitions.push_back(command);
    }
    for (const ExtractedSimulationEventDefinition& input : extracted.simulationEvents) {
        SimulationEventDefinition event{};
        if (!text_literal(input.name, event.name)) {
            return false;
        }
        event.eventType = input.eventType;
        event.primarySchema = input.primarySchema;
        event.secondarySchema = input.secondarySchema;
        event.descriptorEvidenceAddress = input.descriptorEvidenceAddress;
        event.primaryEvidenceAddress = input.primaryEvidenceAddress;
        event.secondaryEvidenceAddress = input.secondaryEvidenceAddress;
        event.flags = format::kSimulationEventDefinitionExact;
        if (input.primarySchema == format::kAbsentIndex) {
            event.flags |= format::kSimulationEventPrimaryAbsent;
        }
        if (input.secondarySchema == format::kAbsentIndex) {
            event.flags |= format::kSimulationEventSecondaryAbsent;
        }
        snapshot.simulationEvents.push_back(event);
    }
    for (const ExtractedRuntimeSchema& input : extracted.runtimeSchemas) {
        RuntimeSchema schema{};
        schema.handle = input.handle;
        schema.decodedSize = input.decodedSize;
        schema.definitionHash = input.definitionHash;
        schema.definitionClass = input.definitionClass;
        schema.codecFamilies = input.codecFamilies;
        schema.fields = {input.firstField, input.fieldCount};
        schema.evidenceAddress = input.evidenceAddress;
        schema.flags = input.flags;
        schema.arrayElementCount = input.arrayElementCount;
        snapshot.runtimeSchemas.push_back(schema);
    }
    for (const ExtractedRuntimeField& input : extracted.runtimeFields) {
        RuntimeField field{};
        field.schemaIndex = input.schemaIndex;
        field.ordinal = input.ordinal;
        field.structOffset = input.structOffset;
        field.alternateOffset = input.alternateOffset;
        field.typeCode = input.typeCode;
        field.nestedHandle = input.nestedHandle;
        field.bias = input.bias;
        field.bits = input.bits;
        field.codecParameters = input.codecParameters;
        field.flags = input.flags;
        snapshot.runtimeFields.push_back(field);
    }
    for (const ExtractedRuntimeTypeDefinition& input : extracted.runtimeTypes) {
        RuntimeTypeDefinition type{};
        if (!text_literal(input.name, type.name)) {
            return false;
        }
        type.codecFamilies = input.codecFamily;
        type.typeCode = input.typeCode;
        type.decodedSize = input.decodedSize;
        type.fixedBits = input.fixedBits;
        type.minimumBits = input.minimumBits;
        type.maximumBits = input.maximumBits;
        type.writerEvidenceAddress = input.writerEvidenceAddress;
        type.readerEvidenceAddress = input.readerEvidenceAddress;
        type.flags = input.flags;
        snapshot.runtimeTypes.push_back(type);
    }
    for (const ExtractedEntityTypeDefinition& input : extracted.entityTypes) {
        EntityTypeDefinition entity{};
        if (!text_literal(input.name, entity.name)) {
            return false;
        }
        entity.entityType = input.entityType;
        entity.baselineSchema = input.baselineSchema;
        entity.updateSchema = input.updateSchema;
        entity.vtableEvidenceAddress = input.vtableEvidenceAddress;
        entity.baselineEvidenceAddress = input.baselineEvidenceAddress;
        entity.updateEvidenceAddress = input.updateEvidenceAddress;
        entity.flags = input.flags;
        snapshot.entityTypes.push_back(entity);
    }
    return true;
}

/** Validates every id, scalar, raw row, owner, range, order, join, and flag. */
bool validate(const Snapshot& snapshot) noexcept {
    if (!sequence_inventory::validate(snapshot)) return false;
    if (!snapshot.complete || snapshot.actorClasses.empty()) {
        return false;
    }

    Text messageName{};
    const ExtractedActorEngineSemantics& extracted = kExtractedActorEngineSemantics;
    if (snapshot.messageSchemas.size() != 1
        || snapshot.commandDefinitions.size() != extracted.commands.size()
        || snapshot.behaviorProfiles.size() != snapshot.actorClasses.size()
        || !text_literal(extracted.message.name, messageName)) {
        return false;
    }
    const ActorMessageSchema& message = snapshot.messageSchemas.front();
    if (!same_text(message.name, messageName)
        || message.definitionHandle != extracted.message.definitionHandle
        || message.durableKey != extracted.message.durableKey
        || message.ownerClass != extracted.message.ownerClass
        || message.handlerSlot != extracted.message.handlerSlot
        || message.bodyType != extracted.message.bodyType
        || message.provenance != format::ActorSemanticProvenance::executableStatic
        || message.commands.first != 0
        || message.commands.count != snapshot.commandDefinitions.size()
        || message.flags != format::kActorMessageSchemaExact) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.commandDefinitions.size(); ++index) {
        const ActorCommandDefinition& command = snapshot.commandDefinitions[index];
        const ExtractedActorCommandDefinition& expected = extracted.commands[index];
        Text commandName{};
        Text factionNoneName{};
        Text factionRemovedName{};
        Text factionHostileName{};
        const auto expectedEffect = expected.setFaction ? format::ActorCommandEffect::setFaction
                                                        : format::ActorCommandEffect::opaque;
        if (!text_literal(expected.name, commandName)
            || (expected.setFaction
                && (!text_literal(expected.factionNoneName, factionNoneName)
                    || !text_literal(expected.factionRemovedName, factionRemovedName)
                    || !text_literal(expected.factionHostileToAllName, factionHostileName)))
            || !same_text(command.name, commandName)
            || !same_text(command.factionNoneName, factionNoneName)
            || !same_text(command.factionRemovedName, factionRemovedName)
            || !same_text(command.factionHostileToAllName, factionHostileName)
            || command.selector != expected.selector
            || command.payloadHandle != expected.payloadHandle || command.effect != expectedEffect
            || command.provenance != format::ActorSemanticProvenance::executableStatic
            || command.factionNone != expected.factionNone
            || command.factionRemoved != expected.factionRemoved
            || command.factionHostileToAll != expected.factionHostileToAll
            || command.flags != format::kActorCommandDefinitionExact) {
            return false;
        }
    }
    if (snapshot.simulationEvents.size() != extracted.simulationEvents.size()
        || snapshot.runtimeSchemas.size() != extracted.runtimeSchemas.size()
        || snapshot.runtimeFields.size() != extracted.runtimeFields.size()
        || snapshot.runtimeTypes.size() != extracted.runtimeTypes.size()
        || snapshot.entityTypes.size() != extracted.entityTypes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.simulationEvents.size(); ++index) {
        const SimulationEventDefinition& row = snapshot.simulationEvents[index];
        const ExtractedSimulationEventDefinition& expected = extracted.simulationEvents[index];
        Text expectedName{};
        const std::uint32_t absentFlags =
            (expected.primarySchema == format::kAbsentIndex ? format::kSimulationEventPrimaryAbsent
                                                            : 0U)
            | (expected.secondarySchema == format::kAbsentIndex
                   ? format::kSimulationEventSecondaryAbsent
                   : 0U);
        if (!text_literal(expected.name, expectedName) || !same_text(row.name, expectedName)
            || row.eventType != expected.eventType || row.primarySchema != expected.primarySchema
            || row.secondarySchema != expected.secondarySchema
            || row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.descriptorEvidenceAddress != expected.descriptorEvidenceAddress
            || row.primaryEvidenceAddress != expected.primaryEvidenceAddress
            || row.secondaryEvidenceAddress != expected.secondaryEvidenceAddress
            || row.flags != (format::kSimulationEventDefinitionExact | absentFlags)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.runtimeSchemas.size(); ++index) {
        const RuntimeSchema& row = snapshot.runtimeSchemas[index];
        const ExtractedRuntimeSchema& expected = extracted.runtimeSchemas[index];
        if (row.handle != expected.handle || row.decodedSize != expected.decodedSize
            || row.definitionHash != expected.definitionHash
            || row.definitionClass != expected.definitionClass
            || row.codecFamilies != expected.codecFamilies
            || row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.fields.first != expected.firstField || row.fields.count != expected.fieldCount
            || !range_inside(row.fields, snapshot.runtimeFields.size())
            || row.evidenceAddress != expected.evidenceAddress || row.flags != expected.flags
            || row.arrayElementCount != expected.arrayElementCount) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.runtimeFields.size(); ++index) {
        const RuntimeField& row = snapshot.runtimeFields[index];
        const ExtractedRuntimeField& expected = extracted.runtimeFields[index];
        if (row.schemaIndex != expected.schemaIndex || row.ordinal != expected.ordinal
            || row.structOffset != expected.structOffset
            || row.alternateOffset != expected.alternateOffset || row.typeCode != expected.typeCode
            || row.nestedHandle != expected.nestedHandle || row.bias != expected.bias
            || row.bits != expected.bits || row.codecParameters != expected.codecParameters
            || row.flags != expected.flags || row.schemaIndex >= snapshot.runtimeSchemas.size()) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.runtimeTypes.size(); ++index) {
        const RuntimeTypeDefinition& row = snapshot.runtimeTypes[index];
        const ExtractedRuntimeTypeDefinition& expected = extracted.runtimeTypes[index];
        Text expectedName{};
        if (!text_literal(expected.name, expectedName) || !same_text(row.name, expectedName)
            || row.codecFamilies != expected.codecFamily || row.typeCode != expected.typeCode
            || row.decodedSize != expected.decodedSize || row.fixedBits != expected.fixedBits
            || row.minimumBits != expected.minimumBits || row.maximumBits != expected.maximumBits
            || row.writerEvidenceAddress != expected.writerEvidenceAddress
            || row.readerEvidenceAddress != expected.readerEvidenceAddress
            || row.flags != expected.flags) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.entityTypes.size(); ++index) {
        const EntityTypeDefinition& row = snapshot.entityTypes[index];
        const ExtractedEntityTypeDefinition& expected = extracted.entityTypes[index];
        Text expectedName{};
        if (!text_literal(expected.name, expectedName) || !same_text(row.name, expectedName)
            || row.entityType != expected.entityType
            || row.baselineSchema != expected.baselineSchema
            || row.updateSchema != expected.updateSchema
            || row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.vtableEvidenceAddress != expected.vtableEvidenceAddress
            || row.baselineEvidenceAddress != expected.baselineEvidenceAddress
            || row.updateEvidenceAddress != expected.updateEvidenceAddress
            || row.flags != expected.flags) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.actorStateNames.size(); ++index) {
        const ActorStateName& row = snapshot.actorStateNames[index];
        const bool ordered =
            index == 0 || snapshot.actorStateNames[index - 1U].actorClassIndex < row.actorClassIndex
            || (snapshot.actorStateNames[index - 1U].actorClassIndex == row.actorClassIndex
                && snapshot.actorStateNames[index - 1U].ordinal + 1U == row.ordinal);
        const bool first =
            index == 0
            || snapshot.actorStateNames[index - 1U].actorClassIndex != row.actorClassIndex;
        if (!ordered || (first && row.ordinal != 0)
            || row.actorClassIndex >= snapshot.actorClasses.size()
            || is_absent_tag(row.definitionTag)
            || row.groupHash != format::kActorStateMachineGroupHash || row.nameHash == 0
            || row.nameHash == kAbsentTag || row.flags != format::kActorStateNameExact) {
            return false;
        }
    }
    if (snapshot.sobjectRsatFieldBindings.size() != snapshot.fields.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.sobjectRsatFieldBindings.size(); ++index) {
        const SobjectRsatFieldBinding& binding = snapshot.sobjectRsatFieldBindings[index];
        const RsatField& field = snapshot.fields[index];
        std::uint32_t handle = 0;
        std::uint32_t parameter14 = 0;
        std::uint32_t parameter18 = 0;
        std::uint64_t decodedOffset = 0;
        std::memcpy(&handle, field.rawRow.data() + 0x10U, sizeof handle);
        std::memcpy(&parameter14, field.rawRow.data() + 0x14U, sizeof parameter14);
        std::memcpy(&parameter18, field.rawRow.data() + 0x18U, sizeof parameter18);
        std::memcpy(&decodedOffset, field.rawRow.data() + 0x20U, sizeof decodedOffset);
        const std::uint32_t expectedFlags =
            format::kSobjectRsatFieldBindingExact
            | (handle != format::kAbsentIndex ? format::kSobjectRsatFieldBindingHasRuntimeSchema
                                              : 0U);
        const RuntimeSchema* runtime = nullptr;
        if (handle != format::kAbsentIndex) {
            const auto found =
                std::find_if(snapshot.runtimeSchemas.begin(),
                             snapshot.runtimeSchemas.end(),
                             [handle](const RuntimeSchema& row) { return row.handle == handle; });
            if (found == snapshot.runtimeSchemas.end()) {
                return false;
            }
            runtime = &*found;
        }
        if (binding.rsatFieldIndex != index || binding.runtimeSchemaHandle != handle
            || binding.parameter14 != parameter14 || binding.parameter18 != parameter18
            || binding.decodedOffset != decodedOffset
            || (runtime != nullptr
                && (binding.definitionClass != runtime->definitionClass
                    || binding.codecFamilies != runtime->codecFamilies))
            || (runtime == nullptr && (binding.definitionClass != 0 || binding.codecFamilies != 0))
            || binding.provenance != format::ActorSemanticProvenance::packageField
            || binding.flags != expectedFlags) {
            return false;
        }
    }

    std::vector<std::uint32_t> schemaReferences{};
    std::size_t descriptorCursor = 0;
    try {
        schemaReferences.resize(snapshot.schemas.size());
    } catch (...) {
        return false;
    }
    for (std::size_t actorIndex = 0; actorIndex < snapshot.actorClasses.size(); ++actorIndex) {
        const ActorClass& actor = snapshot.actorClasses[actorIndex];
        const ActorBehaviorProfile& profile = snapshot.behaviorProfiles[actorIndex];
        Text expectedId{};
        const bool absentRsat = is_absent_tag(actor.rsatTag);
        const bool nullArray = !absentRsat && actor.descriptorArrayRelative == 0;
        const bool typedArray =
            !absentRsat
            && stored_typed_array(actor.descriptorArrayOffset,
                                  actor.descriptorArrayRelative,
                                  actor.descriptorArrayHeaderOffset,
                                  actor.descriptorArrayDataOffset)
            && actor.descriptorElementClass == format::kActorRsatDescriptorClass;
        if (!format_actor_id(actor.definitionTag, expectedId) || !same_text(actor.id, expectedId)
            || is_absent_tag(actor.definitionTag)
            || (actorIndex != 0
                && snapshot.actorClasses[actorIndex - 1U].definitionTag >= actor.definitionTag)
            || actor.objectType > 0xFFU || !valid_authored_spawn_profile(actor)
            || profile.actorClassIndex != actorIndex
            || (is_absent_tag(profile.behaviorConfigTag)
                && (profile.behaviorConfigClass != format::kAbsentIndex
                    || profile.behaviorProvenance != format::ActorSemanticProvenance::notPresent))
            || (!is_absent_tag(profile.behaviorConfigTag)
                && (profile.behaviorConfigClass != format::kActorBehaviorConfigClass
                    || profile.behaviorProvenance != format::ActorSemanticProvenance::packageField))
            || profile.behaviorConfigOffset != kActorBehaviorConfigOffset
            || profile.defaultFaction != 0
            || profile.factionProvenance != format::ActorSemanticProvenance::engineZeroDefault
            || profile.flags != format::kActorBehaviorProfileExact
            || actor.descriptors.first != descriptorCursor
            || !range_inside(actor.descriptors, snapshot.descriptors.size())
            || actor.dynamicPresenceTailCount > actor.descriptors.count
            || (absentRsat
                && (actor.rsatReverseDefinitionTag != format::kAbsentIndex
                    || actor.descriptorArrayOffset != format::kAbsentIndex
                    || actor.descriptorArrayRelative != format::kAbsentRelativeOffset
                    || actor.descriptorArrayHeaderOffset != format::kAbsentIndex
                    || actor.descriptorArrayDataOffset != format::kAbsentIndex
                    || actor.descriptorElementClass != format::kAbsentIndex
                    || actor.descriptors.count != 0 || actor.dynamicPresenceTailCount != 0))
            || (!absentRsat
                && (actor.rsatReverseDefinitionTag != actor.definitionTag
                    || actor.descriptorArrayOffset != format::kActorRsatDescriptorArrayOffset
                    || (!nullArray && !typedArray)
                    || (nullArray
                        && (actor.descriptorArrayHeaderOffset != format::kAbsentIndex
                            || actor.descriptorArrayDataOffset != format::kAbsentIndex
                            || actor.descriptorElementClass != format::kAbsentIndex
                            || actor.descriptors.count != 0
                            || actor.dynamicPresenceTailCount != 0))))) {
            return false;
        }
        if (!absentRsat) {
            for (std::size_t priorActorIndex = 0; priorActorIndex < actorIndex; ++priorActorIndex) {
                if (snapshot.actorClasses[priorActorIndex].rsatTag == actor.rsatTag) {
                    return false;
                }
            }
        }

        std::uint32_t tailOrdinal = 0;
        for (std::uint32_t ordinal = 0; ordinal < actor.descriptors.count; ++ordinal) {
            const RsatDescriptor& descriptor = snapshot.descriptors[descriptorCursor + ordinal];
            Text expectedDescriptorId{};
            const bool eligible =
                (descriptor.flags & format::kRsatDescriptorDynamicPresenceEligible) != 0;
            const std::uint64_t offset =
                static_cast<std::uint64_t>(actor.descriptorArrayDataOffset)
                + static_cast<std::uint64_t>(ordinal) * format::kRsatDescriptorRawRowSize;
            std::uint32_t rawComponent = 0;
            std::uint32_t rawSchema = 0;
            std::memcpy(&rawComponent, descriptor.rawRow.data(), sizeof rawComponent);
            std::memcpy(&rawSchema, descriptor.rawRow.data() + 4U, sizeof rawSchema);
            if (!format_descriptor_id(actor.rsatTag, ordinal, expectedDescriptorId)
                || !same_text(descriptor.id, expectedDescriptorId)
                || descriptor.actorClassIndex != actorIndex || descriptor.rsatTag != actor.rsatTag
                || descriptor.descriptorOrdinal != ordinal
                || offset > (std::numeric_limits<std::uint32_t>::max)()
                || descriptor.descriptorOffset != static_cast<std::uint32_t>(offset)
                || descriptor.descriptorElementClass != format::kActorRsatDescriptorClass
                || descriptor.schemaIndex >= snapshot.schemas.size()
                || (descriptor.flags & ~format::kRsatDescriptorFlagMask) != 0
                || descriptor.componentTag != rawComponent || descriptor.schemaTag != rawSchema
                || (eligible && descriptor.dynamicPresenceTailOrdinal != tailOrdinal)
                || (!eligible && descriptor.dynamicPresenceTailOrdinal != format::kAbsentIndex)) {
                return false;
            }
            const RsatSchema& schema = snapshot.schemas[descriptor.schemaIndex];
            const bool schemaEligible =
                (schema.flags & format::kRsatSchemaDynamicPresenceEligible) != 0;
            if (descriptor.schemaTag != schema.schemaTag
                || descriptor.schemaFieldCount != schema.fieldCount
                || descriptor.schemaFirstFieldRuntimeGate != schema.firstFieldRuntimeGate
                || descriptor.schemaFirstFieldRawU32At10 != schema.firstFieldRawU32At10
                || eligible != schemaEligible) {
                return false;
            }
            ++schemaReferences[descriptor.schemaIndex];
            tailOrdinal += eligible ? 1U : 0U;
        }
        if (tailOrdinal != actor.dynamicPresenceTailCount) {
            return false;
        }
        descriptorCursor += actor.descriptors.count;
    }
    if (descriptorCursor != snapshot.descriptors.size()) {
        return false;
    }

    std::size_t sobjectDescriptorCursor = 0;
    for (std::size_t rsatIndex = 0; rsatIndex < snapshot.sobjectRsats.size(); ++rsatIndex) {
        const SobjectRsat& rsat = snapshot.sobjectRsats[rsatIndex];
        const bool nullArray = rsat.descriptorArrayRelative == 0;
        const bool typedArray = stored_typed_array(rsat.descriptorArrayOffset,
                                                   rsat.descriptorArrayRelative,
                                                   rsat.descriptorArrayHeaderOffset,
                                                   rsat.descriptorArrayDataOffset)
                                && rsat.descriptorElementClass == format::kActorRsatDescriptorClass;
        if (is_absent_tag(rsat.rsatTag)
            || (rsatIndex != 0 && snapshot.sobjectRsats[rsatIndex - 1U].rsatTag >= rsat.rsatTag)
            || is_absent_tag(rsat.reverseDefinitionTag)
            || rsat.descriptorArrayOffset != format::kActorRsatDescriptorArrayOffset
            || (!nullArray && !typedArray)
            || (nullArray
                && (rsat.descriptorArrayHeaderOffset != format::kAbsentIndex
                    || rsat.descriptorArrayDataOffset != format::kAbsentIndex
                    || rsat.descriptorElementClass != format::kAbsentIndex
                    || rsat.descriptors.count != 0 || rsat.dynamicPresenceTailCount != 0))
            || rsat.provenance != format::ActorSemanticProvenance::packageField
            || rsat.descriptors.first != sobjectDescriptorCursor
            || !range_inside(rsat.descriptors, snapshot.sobjectRsatDescriptors.size())
            || rsat.dynamicPresenceTailCount > rsat.descriptors.count
            || rsat.flags != format::kSobjectRsatExact || rsat.reserved != 0) {
            return false;
        }
        std::uint32_t tailOrdinal = 0;
        for (std::uint32_t ordinal = 0; ordinal < rsat.descriptors.count; ++ordinal) {
            const SobjectRsatDescriptor& descriptor =
                snapshot.sobjectRsatDescriptors[sobjectDescriptorCursor + ordinal];
            const std::uint64_t expectedOffset =
                static_cast<std::uint64_t>(rsat.descriptorArrayDataOffset)
                + static_cast<std::uint64_t>(ordinal) * format::kRsatDescriptorRawRowSize;
            std::uint32_t rawComponent = 0;
            std::uint32_t rawSchema = 0;
            std::memcpy(&rawComponent, descriptor.rawRow.data(), sizeof rawComponent);
            std::memcpy(&rawSchema, descriptor.rawRow.data() + 4U, sizeof rawSchema);
            const bool eligible =
                (descriptor.flags & format::kSobjectRsatDescriptorDynamicPresenceEligible) != 0;
            if (descriptor.rsatIndex != rsatIndex || descriptor.descriptorOrdinal != ordinal
                || expectedOffset > (std::numeric_limits<std::uint32_t>::max)()
                || descriptor.descriptorOffset != static_cast<std::uint32_t>(expectedOffset)
                || descriptor.componentTag != rawComponent || descriptor.schemaTag != rawSchema
                || descriptor.schemaIndex >= snapshot.schemas.size()
                || descriptor.schemaTag != snapshot.schemas[descriptor.schemaIndex].schemaTag
                || descriptor.schemaFieldCount
                       != snapshot.schemas[descriptor.schemaIndex].fieldCount
                || descriptor.schemaFirstFieldRuntimeGate
                       != snapshot.schemas[descriptor.schemaIndex].firstFieldRuntimeGate
                || (eligible && descriptor.dynamicPresenceTailOrdinal != tailOrdinal)
                || (!eligible && descriptor.dynamicPresenceTailOrdinal != format::kAbsentIndex)
                || (descriptor.flags & ~format::kSobjectRsatDescriptorDynamicPresenceEligible)
                       != 0) {
                return false;
            }
            ++schemaReferences[descriptor.schemaIndex];
            tailOrdinal += eligible ? 1U : 0U;
        }
        if (tailOrdinal != rsat.dynamicPresenceTailCount) {
            return false;
        }
        sobjectDescriptorCursor += rsat.descriptors.count;
    }
    if (sobjectDescriptorCursor != snapshot.sobjectRsatDescriptors.size()
        || snapshot.sobjectRsats.empty()) {
        return false;
    }

    std::size_t fieldCursor = 0;
    for (std::size_t schemaIndex = 0; schemaIndex < snapshot.schemas.size(); ++schemaIndex) {
        const RsatSchema& schema = snapshot.schemas[schemaIndex];
        Text expectedId{};
        const bool typed = (schema.flags & format::kRsatSchemaTypedFieldArray) != 0;
        const bool eligible = (schema.flags & format::kRsatSchemaDynamicPresenceEligible) != 0;
        const bool typedShape = stored_typed_array(schema.fieldArrayOffset,
                                                   schema.fieldArrayRelative,
                                                   schema.fieldArrayHeaderOffset,
                                                   schema.fieldArrayDataOffset)
                                && schema.fieldElementClass == format::kActorRsatSchemaFieldClass;
        if (!format_schema_id(schema.schemaTag, expectedId) || !same_text(schema.id, expectedId)
            || is_absent_tag(schema.schemaTag)
            || (schemaIndex != 0
                && snapshot.schemas[schemaIndex - 1U].schemaTag >= schema.schemaTag)
            || schema.schemaClass != format::kActorRsatSchemaClass
            || schema.fieldCount != schema.fields.count || schema.fields.first != fieldCursor
            || !range_inside(schema.fields, snapshot.fields.size())
            || schema.fieldArrayOffset != format::kActorRsatSchemaFieldArrayOffset
            || (schema.flags & ~format::kRsatSchemaFlagMask) != 0
            || eligible
                   != (schema.fieldCount != 0
                       && schema.firstFieldRuntimeGate != format::kAbsentIndex)
            || (schema.fieldCount == 0
                && (schema.firstFieldRuntimeGate != format::kAbsentIndex
                    || schema.firstFieldRawU32At10 != format::kAbsentIndex))
            || (!typed
                && (schema.fieldCount != 0 || schema.fieldArrayRelative != 0
                    || schema.fieldArrayHeaderOffset != format::kAbsentIndex
                    || schema.fieldArrayDataOffset != format::kAbsentIndex
                    || schema.fieldElementClass != format::kAbsentIndex))
            || (typed && !typedShape) || schemaReferences[schemaIndex] == 0) {
            return false;
        }
        if (schema.fieldCount != 0) {
            std::uint32_t firstGate = 0;
            std::uint32_t firstRaw = 0;
            const RsatField& first = snapshot.fields[fieldCursor];
            std::memcpy(&firstGate, first.rawRow.data(), sizeof firstGate);
            std::memcpy(&firstRaw, first.rawRow.data() + 0x10U, sizeof firstRaw);
            if (firstGate != schema.firstFieldRuntimeGate
                || firstRaw != schema.firstFieldRawU32At10) {
                return false;
            }
        }
        fieldCursor += schema.fieldCount;
    }
    return fieldCursor == snapshot.fields.size();
}

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
