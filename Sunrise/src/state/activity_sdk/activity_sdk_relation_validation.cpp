#include <array>
#include <cstddef>
#include <cstdio>

#include "actor_sequences.h"
#include "internal.h"
#include "validation_internal.h"

namespace sunrise::state::activity_sdk::validation {
namespace {

/** @return True when every task target names slots and objectives the catalog holds. */
[[nodiscard]] bool task_targets(const Catalog& catalog) noexcept {
    const auto targets = catalog.task_targets();
    const auto slots = catalog.slots();
    std::uint32_t previousTask{};
    bool first = true;
    for (const format::TaskTarget& row : targets) {
        if (row.taskSlotIndex >= slots.size() || row.objectiveSlotIndex >= slots.size()
            || row.flags != format::kTaskTargetExact || row.reserved != 0
            || row.referenceFieldOffset
                   != row.descriptorOffset + format::kTaskReferenceRelativeOffset
            || row.bitIndex >= 24U || (!first && row.taskSlotIndex < previousTask)) {
            return false;
        }
        const format::Slot& task = slots[row.taskSlotIndex];
        const format::Slot& objective = slots[row.objectiveSlotIndex];
        if (task.slotType != format::kTaskSlotType
            || task.componentClass != format::kTaskComponentClass
            || task.authSchema != format::kTaskAuthSchema
            || objective.slotType != format::kObjectiveSlotType
            || objective.componentClass != format::kObjectiveComponentClass
            || objective.senseSchema != format::kObjectiveSenseSchema
            || objective.authSchema != format::kObjectiveAuthSchema
            || objective.objectIndex >= catalog.objects().size()
            || catalog.objects()[objective.objectIndex].objectKey != row.targetObjectKey) {
            return false;
        }
        previousTask = row.taskSlotIndex;
        first = false;
    }
    return true;
}

/** @return True when every authored text row points inside its own string section. */
[[nodiscard]] bool authored_text(const Catalog& catalog) noexcept {
    const auto slots = catalog.slots();
    for (const format::DialogueCueText& row : catalog.dialogue_cue_texts()) {
        if (row.slotIndex >= slots.size() || row.cueIndex >= slots[row.slotIndex].reserved
            || slots[row.slotIndex].slotType != format::kDialogueSlotType || row.definitionHash == 0
            || row.definitionHash == 0x811C9DC5U || row.containerTag == 0 || row.stringHash == 0) {
            return false;
        }
    }
    for (const format::DirectiveElement& row : catalog.directive_elements()) {
        if (row.slotIndex >= slots.size() || slots[row.slotIndex].slotType != 68U
            || row.elementIndex < 0
            || static_cast<std::uint32_t>(row.elementIndex) >= row.elementCount
            || row.titleContainerTag == 0 || row.titleStringHash == 0
            || row.descriptionContainerTag == 0 || row.descriptionStringHash == 0) {
            return false;
        }
    }
    return true;
}

/** @return True when every behavior edge names nodes of its own program. */
[[nodiscard]] bool behavior_edges(const Catalog& catalog) noexcept {
    const std::size_t programs = catalog.behavior_programs().size();
    for (const format::BehaviorInput& row : catalog.behavior_inputs()) {
        if (row.programIndex >= programs || row.reserved != 0) {
            return false;
        }
    }
    for (const format::BehaviorChannelWrite& row : catalog.behavior_channel_writes()) {
        if (row.programIndex >= programs || row.reserved != 0) {
            return false;
        }
    }
    const auto owners = catalog.behavior_owners();
    for (const format::BehaviorOwner& row : owners) {
        if (row.programIndex >= programs || row.actorClassIndex >= catalog.actor_classes().size()) {
            return false;
        }
    }
    for (const format::BehaviorActivityBinding& row : catalog.behavior_activity_bindings()) {
        if (row.ownerIndex >= owners.size() || row.squadIndex >= catalog.squads().size()
            || row.squadMemberIndex >= catalog.squad_members().size()
            || row.scenarioIndex >= catalog.scenarios().size()
            || row.occurrenceIndex >= catalog.occurrences().size()
            || row.stateIndex >= catalog.states().size()
            || row.objectIndex >= catalog.objects().size() || row.reserved != 0) {
            return false;
        }
    }
    return true;
}

/** @return True when actor rows, their commands and their states resolve against each other. */
[[nodiscard]] bool actor_semantics(const Catalog& catalog) noexcept {
    const auto messages = catalog.actor_message_schemas();
    const auto commands = catalog.actor_command_definitions();
    const auto profiles = catalog.actor_behavior_profiles();
    const auto schemas = catalog.runtime_schemas();
    if (messages.size() != 1 || commands.size() != 98
        || profiles.size() != catalog.actor_classes().size()) {
        return false;
    }
    const format::ActorMessageSchema& message = messages.front();
    if (message.definitionHandle == 0 || message.durableKey == 0 || message.ownerClass == 0
        || message.provenance != format::ActorSemanticProvenance::executableStatic
        || message.commands.first != 0 || message.commands.count != commands.size()
        || message.flags != format::kActorMessageSchemaExact || message.reserved != 0) {
        return false;
    }
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const format::ActorCommandDefinition& command = commands[index];
        const bool setFaction = index == 45U;
        if (command.selector != index || command.payloadHandle == 0
            || command.effect
                   != (setFaction ? format::ActorCommandEffect::setFaction
                                  : format::ActorCommandEffect::opaque)
            || command.provenance != format::ActorSemanticProvenance::executableStatic
            || command.flags != format::kActorCommandDefinitionExact
            || (!setFaction
                && (command.factionNoneName.length != 0 || command.factionRemovedName.length != 0
                    || command.factionHostileToAllName.length != 0 || command.factionNone != 0
                    || command.factionRemoved != 0 || command.factionHostileToAll != 0))
            || (setFaction
                && (command.factionNone >= 0 || command.factionRemoved >= 0
                    || command.factionHostileToAll >= 0
                    || command.factionNone == command.factionRemoved
                    || command.factionNone == command.factionHostileToAll
                    || command.factionRemoved == command.factionHostileToAll))) {
            return false;
        }
        if (std::none_of(
                schemas.begin(), schemas.end(), [&command](const format::RuntimeSchema& row) {
                    return row.handle == command.payloadHandle;
                })) {
            return false;
        }
    }
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const format::ActorBehaviorProfile& profile = profiles[index];
        const bool absent =
            profile.behaviorConfigTag == 0 || profile.behaviorConfigTag == format::kAbsentIndex;
        if (profile.actorClassIndex != index
            || (absent
                && (profile.behaviorConfigClass != format::kAbsentIndex
                    || profile.behaviorProvenance != format::ActorSemanticProvenance::notPresent))
            || (!absent
                && (profile.behaviorConfigClass != format::kActorBehaviorConfigClass
                    || profile.behaviorProvenance != format::ActorSemanticProvenance::packageField))
            || profile.behaviorConfigOffset != format::kActorBehaviorConfigRelativeOffset
            || profile.defaultFaction != 0
            || profile.factionProvenance != format::ActorSemanticProvenance::engineZeroDefault
            || profile.flags != format::kActorBehaviorProfileExact) {
            return false;
        }
    }
    return true;
}

/** @return True when every runtime type, schema and codec family agrees with its users. */
[[nodiscard]] bool runtime_semantics(const Catalog& catalog) noexcept {
    const auto events = catalog.simulation_event_definitions();
    const auto schemas = catalog.runtime_schemas();
    const auto fields = catalog.runtime_fields();
    const auto types = catalog.runtime_type_definitions();
    if (events.size() != 22 || schemas.empty() || fields.empty() || types.empty()
        || types.size() % 47 != 0) {
        return false;
    }
    // Every bit a runtime type row may set; any other bit means an unknown extraction.
    constexpr std::uint32_t kTypeFlagMask =
        format::kRuntimeTypeDefinitionExact | format::kRuntimeTypeFixed
        | format::kRuntimeTypeParametric | format::kRuntimeTypeValueDependent
        | format::kRuntimeTypeNullable | format::kRuntimeTypeNested | format::kRuntimeTypeUnion
        | format::kRuntimeTypeResolved | format::kRuntimeTypeRemapped
        | format::kRuntimeTypeUnsupported | format::kRuntimeTypeObfuscated
        | format::kRuntimeTypeSelectorValidationRequired;
    for (std::size_t index = 0; index < types.size(); ++index) {
        const format::RuntimeTypeDefinition& type = types[index];
        const bool fixed = (type.flags & format::kRuntimeTypeFixed) != 0;
        const bool unsupported = (type.flags & format::kRuntimeTypeUnsupported) != 0;
        const bool hasFixedMeasurement = type.fixedBits != format::kAbsentIndex;
        const bool unsupportedNoopMeasurement =
            unsupported && type.fixedBits == 0 && type.minimumBits == 0 && type.maximumBits == 0;
        if ((type.codecFamilies
             & ~(static_cast<std::uint32_t>(format::RuntimeCodecFamily::activity)
                 | static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeZero)
                 | static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeOne)))
                != 0
            || type.codecFamilies == 0 || type.typeCode != index % 47
            || catalog.string(type.name).empty() || type.writerEvidenceAddress == 0
            || type.readerEvidenceAddress == 0 || (type.flags & ~kTypeFlagMask) != 0
            || (type.flags & format::kRuntimeTypeDefinitionExact) == 0 || type.reserved != 0
            || (fixed && !hasFixedMeasurement)
            || (!fixed && hasFixedMeasurement && !unsupportedNoopMeasurement)
            || (fixed && (type.minimumBits != type.fixedBits || type.maximumBits != type.fixedBits))
            || (type.minimumBits != format::kAbsentIndex && type.maximumBits != format::kAbsentIndex
                && type.minimumBits > type.maximumBits)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < events.size(); ++index) {
        const format::SimulationEventDefinition& row = events[index];
        const bool primaryAbsent = row.primarySchema == format::kAbsentIndex;
        const bool secondaryAbsent = row.secondarySchema == format::kAbsentIndex;
        if (row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.descriptorEvidenceAddress == 0
            || primaryAbsent != ((row.flags & format::kSimulationEventPrimaryAbsent) != 0)
            || secondaryAbsent != ((row.flags & format::kSimulationEventSecondaryAbsent) != 0)
            || (!primaryAbsent
                && (row.primaryEvidenceAddress == 0
                    || runtime_schema_by_handle(catalog, row.primarySchema) == nullptr))
            || (!secondaryAbsent
                && (row.secondaryEvidenceAddress == 0
                    || runtime_schema_by_handle(catalog, row.secondarySchema) == nullptr))
            || (row.flags
                & ~(format::kSimulationEventDefinitionExact | format::kSimulationEventPrimaryAbsent
                    | format::kSimulationEventSecondaryAbsent))
                   != 0
            || row.reserved != 0) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (events[prior].eventType == row.eventType
                || catalog.string(events[prior].name) == catalog.string(row.name)) {
                return false;
            }
        }
    }
    std::size_t fieldCursor = 0;
    for (std::size_t index = 0; index < schemas.size(); ++index) {
        const format::RuntimeSchema& schema = schemas[index];
        if (schema.handle == 0 || schema.handle == format::kAbsentIndex
            || (schema.decodedSize == 0 && schema.fields.count != 0) || schema.definitionHash == 0
            || schema.definitionClass == format::kAbsentIndex || schema.codecFamilies == 0
            || (schema.codecFamilies
                & ~(static_cast<std::uint32_t>(format::RuntimeCodecFamily::activity)
                    | static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeZero)
                    | static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeOne)))
                   != 0
            || schema.provenance != format::ActorSemanticProvenance::executableStatic
            || schema.fields.first != fieldCursor
            || schema.fields.count > fields.size() - fieldCursor || schema.evidenceAddress == 0
            || (schema.flags & ~(format::kRuntimeSchemaExact | format::kRuntimeSchemaArrayRegion))
                   != 0
            || (schema.flags & format::kRuntimeSchemaExact) == 0
            || (schema.arrayElementCount != 0)
                   != ((schema.flags & format::kRuntimeSchemaArrayRegion) != 0)
            || (index != 0 && schemas[index - 1U].handle >= schema.handle)) {
            return false;
        }
        for (std::uint32_t ordinal = 0; ordinal < schema.fields.count; ++ordinal) {
            const format::RuntimeField& field = fields[fieldCursor + ordinal];
            if (field.schemaIndex != index || field.ordinal != ordinal
                || field.structOffset >= schema.decodedSize
                || (field.flags
                    & ~(format::kRuntimeFieldExact | format::kRuntimeFieldPresenceBit
                        | format::kRuntimeFieldDynamicActorCommand
                        | format::kRuntimeFieldNestedSchema | format::kRuntimeFieldDynamicArray
                        | format::kRuntimeFieldCustomCodec))
                       != 0
                || (field.flags & format::kRuntimeFieldExact) == 0
                || ((field.nestedHandle != format::kAbsentIndex)
                    != ((field.flags & format::kRuntimeFieldNestedSchema) != 0))
                || ((field.flags & format::kRuntimeFieldDynamicArray) != 0
                    && field.typeCode
                           != static_cast<std::uint32_t>(format::RuntimeFieldType::nested))
                || (schema.codecFamilies
                    & static_cast<std::uint32_t>(format::RuntimeCodecFamily::activity))
                           != 0
                       && runtime_type_by_code(
                              catalog, format::RuntimeCodecFamily::activity, field.typeCode)
                              == nullptr
                || (schema.codecFamilies
                    & static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeZero))
                           != 0
                       && runtime_type_by_code(
                              catalog, format::RuntimeCodecFamily::sobjectModeZero, field.typeCode)
                              == nullptr
                || (schema.codecFamilies
                    & static_cast<std::uint32_t>(format::RuntimeCodecFamily::sobjectModeOne))
                           != 0
                       && runtime_type_by_code(
                              catalog, format::RuntimeCodecFamily::sobjectModeOne, field.typeCode)
                              == nullptr) {
                return false;
            }
            if (field.nestedHandle != format::kAbsentIndex
                && runtime_schema_by_handle(catalog, field.nestedHandle) == nullptr) {
                return false;
            }
        }
        fieldCursor += schema.fields.count;
    }
    if (fieldCursor != fields.size()) {
        return false;
    }
    const format::ActorMessageSchema& message = catalog.actor_message_schemas().front();
    const auto command =
        std::find_if(catalog.actor_command_definitions().begin(),
                     catalog.actor_command_definitions().end(),
                     [](const format::ActorCommandDefinition& row) {
                         return row.effect == format::ActorCommandEffect::setFaction;
                     });
    if (command == catalog.actor_command_definitions().end()) {
        return false;
    }
    const format::RuntimeSchema* const messageSchema =
        runtime_schema_by_handle(catalog, message.definitionHandle);
    const format::RuntimeSchema* const payloadSchema =
        runtime_schema_by_handle(catalog, command->payloadHandle);
    if (messageSchema == nullptr || payloadSchema == nullptr) {
        return false;
    }
    const auto messageFields = runtime_schema_fields(catalog, *messageSchema);
    const auto payloadFields = runtime_schema_fields(catalog, *payloadSchema);
    return messageFields.size() == 1 && messageFields.front().typeCode == message.bodyType
           && (messageFields.front().flags & format::kRuntimeFieldDynamicActorCommand) != 0
           && payloadFields.size() == 1 && payloadFields.front().typeCode == 5
           && payloadFields.front().bits == 32;
}

/** @return True when every SObject row, its RSAT and its descriptors resolve. */
[[nodiscard]] bool sobject_semantics(const Catalog& catalog) noexcept {
    const auto rsats = catalog.sobject_rsats();
    const auto descriptors = catalog.sobject_rsat_descriptors();
    const auto schemas = catalog.rsat_schemas();
    const auto fields = catalog.rsat_fields();
    const auto bindings = catalog.sobject_rsat_field_bindings();
    if (rsats.empty() || descriptors.empty() || bindings.size() != fields.size()) {
        return false;
    }
    std::size_t descriptorCursor = 0;
    for (std::size_t rsatIndex = 0; rsatIndex < rsats.size(); ++rsatIndex) {
        const format::SobjectRsat& rsat = rsats[rsatIndex];
        if (rsat.rsatTag == 0 || rsat.rsatTag == format::kAbsentIndex
            || rsat.reverseDefinitionTag == 0 || rsat.reverseDefinitionTag == format::kAbsentIndex
            || rsat.provenance != format::ActorSemanticProvenance::packageField
            || rsat.descriptors.first != descriptorCursor || rsat.flags != format::kSobjectRsatExact
            || rsat.reserved != 0
            || (rsatIndex != 0 && rsats[rsatIndex - 1U].rsatTag >= rsat.rsatTag)) {
            return false;
        }
        std::uint32_t tailOrdinal = 0;
        for (std::uint32_t ordinal = 0; ordinal < rsat.descriptors.count; ++ordinal) {
            const format::SobjectRsatDescriptor& descriptor =
                descriptors[descriptorCursor + ordinal];
            const bool dynamic =
                (descriptor.flags & format::kSobjectRsatDescriptorDynamicPresenceEligible) != 0;
            if (descriptor.rsatIndex != rsatIndex || descriptor.descriptorOrdinal != ordinal
                || descriptor.schemaIndex >= schemas.size()
                || descriptor.schemaTag != schemas[descriptor.schemaIndex].schemaTag
                || descriptor.schemaFieldCount != schemas[descriptor.schemaIndex].fieldCount
                || descriptor.schemaFirstFieldRuntimeGate
                       != schemas[descriptor.schemaIndex].firstFieldRuntimeGate
                || (dynamic && descriptor.dynamicPresenceTailOrdinal != tailOrdinal)
                || (!dynamic && descriptor.dynamicPresenceTailOrdinal != format::kAbsentIndex)) {
                return false;
            }
            tailOrdinal += dynamic ? 1U : 0U;
        }
        if (tailOrdinal != rsat.dynamicPresenceTailCount) {
            return false;
        }
        descriptorCursor += rsat.descriptors.count;
    }
    if (descriptorCursor != descriptors.size()) {
        return false;
    }
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const format::SobjectRsatFieldBinding& binding = bindings[index];
        const bool hasSchema = binding.runtimeSchemaHandle != format::kAbsentIndex;
        const format::RuntimeSchema* const runtime =
            hasSchema ? runtime_schema_by_handle(catalog, binding.runtimeSchemaHandle) : nullptr;
        if (binding.rsatFieldIndex != index
            || binding.provenance != format::ActorSemanticProvenance::packageField
            || (binding.flags & format::kSobjectRsatFieldBindingExact) == 0
            || hasSchema
                   != ((binding.flags & format::kSobjectRsatFieldBindingHasRuntimeSchema) != 0)
            || (hasSchema
                && (runtime == nullptr || binding.definitionClass != runtime->definitionClass
                    || binding.codecFamilies != runtime->codecFamilies))
            || (!hasSchema && (binding.definitionClass != 0 || binding.codecFamilies != 0))) {
            return false;
        }
    }
    return true;
}

/** @return True when every entity type names a live schema and a unique generated name. */
[[nodiscard]] bool entity_types(const Catalog& catalog) noexcept {
    const auto rows = catalog.entity_type_definitions();
    if (rows.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const format::EntityTypeDefinition& row = rows[index];
        const bool baselinePresent = row.baselineSchema != format::kAbsentIndex;
        const bool updatePresent = row.updateSchema != format::kAbsentIndex;
        const bool dynamicUpdate = (row.flags & format::kEntityTypeUpdateUsesSobjectRsat) != 0;
        if (row.provenance != format::ActorSemanticProvenance::executableStatic
            || row.vtableEvidenceAddress == 0
            || baselinePresent != (row.baselineEvidenceAddress != 0)
            || (updatePresent && row.updateEvidenceAddress == 0)
            || ((row.flags & format::kEntityTypeUpdateSupported) != 0 && !updatePresent
                && !dynamicUpdate)
            || (baselinePresent && runtime_schema_by_handle(catalog, row.baselineSchema) == nullptr)
            || (updatePresent && runtime_schema_by_handle(catalog, row.updateSchema) == nullptr)
            || (row.flags
                & ~(format::kEntityTypeDefinitionExact | format::kEntityTypeStockEmittable
                    | format::kEntityTypeUpdateSupported
                    | format::kEntityTypeUpdateUsesSobjectRsat))
                   != 0
            || row.reserved != 0) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (rows[prior].entityType == row.entityType
                || catalog.string(rows[prior].name) == catalog.string(row.name)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * State names are grouped by actor class, contiguous from ordinal zero, and never empty names.
 * The refused row's index reaches the caller so the failure can name it.
 */
[[nodiscard]] bool actor_state_names(const Catalog& catalog, std::size_t& refused) noexcept {
    const auto rows = catalog.actor_state_names();
    const auto actors = catalog.actor_classes();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const format::ActorStateName& row = rows[index];
        const bool first = index == 0 || rows[index - 1U].actorClassIndex != row.actorClassIndex;
        const bool ordered =
            first ? (index == 0 || rows[index - 1U].actorClassIndex < row.actorClassIndex)
                  : rows[index - 1U].ordinal + 1U == row.ordinal;
        if (!ordered || (first && row.ordinal != 0) || row.actorClassIndex >= actors.size()
            || row.definitionTag == 0 || row.definitionTag == format::kAbsentIndex
            || row.groupHash != format::kActorStateMachineGroupHash || row.nameHash == 0
            || row.nameHash == format::kAbsentIndex || row.flags != format::kActorStateNameExact) {
            refused = index;
            return false;
        }
    }
    return true;
}

/** Holds the last refusal so a rejected estate names its cause instead of one opaque status. */
std::array<char, 128> g_reason{};

void remember(const char* check) noexcept {
    const int written = std::snprintf(g_reason.data(), g_reason.size(), "%s", check);
    if (written <= 0) {
        g_reason[0] = '\0';
    }
}

/** Adds the refused row's own fields, which is what tells a producer bug from a wire bug. */
void remember_state_name(const Catalog& catalog, std::size_t refused) noexcept {
    const auto rows = catalog.actor_state_names();
    if (refused >= rows.size()) {
        remember("actor_state_names");
        return;
    }
    const format::ActorStateName& row = rows[refused];
    const int written = std::snprintf(g_reason.data(),
                                      g_reason.size(),
                                      "actor_state_names/row=%zu/actor=%u/actors=%zu/ordinal=%u"
                                      "/name=%08X/def=%08X/group=%08X/flags=%u",
                                      refused,
                                      row.actorClassIndex,
                                      catalog.actor_classes().size(),
                                      row.ordinal,
                                      row.nameHash,
                                      row.definitionTag,
                                      row.groupHash,
                                      row.flags);
    if (written <= 0) {
        remember("actor_state_names");
    }
}

} // namespace

/** Runs every cross-section relation check in order, remembering the first that refuses. */
bool relations(const Catalog& catalog) {
    if (!valid_actor_sequences(catalog)) {
        remember("actor_sequences");
        return false;
    }
    std::size_t refused = 0;
    if (!actor_state_names(catalog, refused)) {
        remember_state_name(catalog, refused);
        return false;
    }
    struct Check final {
        const char* name;
        bool (*run)(const Catalog&) noexcept;
    };
    // Every relation check a catalog must pass, named so a refusal reports which one failed.
    static constexpr std::array<Check, 7> kChecks{{{"task_targets", &task_targets},
                                                   {"authored_text", &authored_text},
                                                   {"behavior_edges", &behavior_edges},
                                                   {"actor_semantics", &actor_semantics},
                                                   {"runtime_semantics", &runtime_semantics},
                                                   {"sobject_semantics", &sobject_semantics},
                                                   {"entity_types", &entity_types}}};
    for (const Check& check : kChecks) {
        if (!check.run(catalog)) {
            remember(check.name);
            return false;
        }
    }
    return true;
}

} // namespace sunrise::state::activity_sdk::validation

namespace sunrise::state::activity_sdk {

/** Validates one catalog: structure first, then relations. @return False on the first refusal. */
bool valid_catalog(const Catalog& value) noexcept {
    try {
        if (!validation::structure(value)) {
            validation::remember("structure");
            return false;
        }
        return validation::relations(value);
    } catch (...) {
        validation::remember("threw");
        return false;
    }
}

const char* last_catalog_reason() noexcept {
    return validation::g_reason.data();
}

} // namespace sunrise::state::activity_sdk
