#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sunrise::state::activity_sdk::format {

/** Eight-byte identity at the start of every runtime SDK pack. */
inline constexpr std::array<char, 8> kMagic{'S', 'R', 'S', 'D', 'K', 'P', '0', '1'};
/** Runtime-pack schema version accepted by this reader. */
inline constexpr std::uint32_t kVersion = 38;
/** The ABI contains only activity identity, topology, placement, and panel metadata. */
inline constexpr std::uint32_t kSectionCount = 46;
#if defined(SUNRISE_ACTIVITY_SDK_TESTING)
/** The runtime accepts only the checked generated SDK build. Each pin is a SHA-256 digest. */
inline constexpr std::array<std::byte, 32> kExpectedSdkBuildDigest{
    std::byte{0x13}, std::byte{0x1A}, std::byte{0xA4}, std::byte{0x2D}, std::byte{0xE2},
    std::byte{0x8E}, std::byte{0xBF}, std::byte{0x73}, std::byte{0x90}, std::byte{0xA1},
    std::byte{0xBD}, std::byte{0x47}, std::byte{0x7C}, std::byte{0xC0}, std::byte{0x16},
    std::byte{0xBF}, std::byte{0x93}, std::byte{0x0E}, std::byte{0x1B}, std::byte{0x00},
    std::byte{0x45}, std::byte{0x9F}, std::byte{0x89}, std::byte{0xDD}, std::byte{0xF4},
    std::byte{0x9F}, std::byte{0xBE}, std::byte{0xED}, std::byte{0x3B}, std::byte{0x42},
    std::byte{0xA3}, std::byte{0x20},
};
/** The runtime accepts only the full generated row projection for this build. */
inline constexpr std::array<std::byte, 32> kExpectedPayloadDigest{
    std::byte{0x39}, std::byte{0xE9}, std::byte{0x03}, std::byte{0x03}, std::byte{0x9B},
    std::byte{0xB6}, std::byte{0x0C}, std::byte{0x08}, std::byte{0xCA}, std::byte{0x1E},
    std::byte{0xCB}, std::byte{0x7D}, std::byte{0x19}, std::byte{0x9A}, std::byte{0x39},
    std::byte{0x24}, std::byte{0xE8}, std::byte{0x3A}, std::byte{0x3F}, std::byte{0xBF},
    std::byte{0x05}, std::byte{0x49}, std::byte{0x41}, std::byte{0x7B}, std::byte{0x7D},
    std::byte{0xDB}, std::byte{0x53}, std::byte{0xC2}, std::byte{0x3F}, std::byte{0x98},
    std::byte{0x0A}, std::byte{0x65},
};
/** The runtime pins the independently replayed package and executable identity. */
inline constexpr std::array<std::byte, 32> kExpectedContentKeyDigest{
    std::byte{0xCE}, std::byte{0xFB}, std::byte{0x02}, std::byte{0xD9}, std::byte{0x24},
    std::byte{0x98}, std::byte{0x18}, std::byte{0x6D}, std::byte{0x0F}, std::byte{0x9A},
    std::byte{0x50}, std::byte{0x9A}, std::byte{0x52}, std::byte{0x10}, std::byte{0x66},
    std::byte{0xD3}, std::byte{0x62}, std::byte{0xBF}, std::byte{0xC4}, std::byte{0x76},
    std::byte{0xE6}, std::byte{0x41}, std::byte{0x06}, std::byte{0xBD}, std::byte{0x2E},
    std::byte{0x53}, std::byte{0x6C}, std::byte{0x71}, std::byte{0x6F}, std::byte{0xB6},
    std::byte{0x0F}, std::byte{0x49},
};
/** The runtime pins the complete normalized logical-IR identity. */
inline constexpr std::array<std::byte, 32> kExpectedLogicalIrDigest{
    std::byte{0x1E}, std::byte{0x91}, std::byte{0xDC}, std::byte{0xBB}, std::byte{0x7D},
    std::byte{0xCD}, std::byte{0x5E}, std::byte{0xE0}, std::byte{0xE5}, std::byte{0x39},
    std::byte{0x2A}, std::byte{0xF9}, std::byte{0xF6}, std::byte{0x79}, std::byte{0x0B},
    std::byte{0x03}, std::byte{0x68}, std::byte{0xA0}, std::byte{0x61}, std::byte{0x93},
    std::byte{0xDB}, std::byte{0xA2}, std::byte{0xC8}, std::byte{0xB8}, std::byte{0xD5},
    std::byte{0xFA}, std::byte{0xC6}, std::byte{0x0A}, std::byte{0x88}, std::byte{0xEB},
    std::byte{0xF6}, std::byte{0x62},
};
#endif
/** UINT32_MAX is the only out-of-band row or optional-string index. */
inline constexpr std::uint32_t kAbsentIndex = 0xFFFFFFFFU;
/** Activity flags retain the three exact joins and complete scenario extraction. */
inline constexpr std::uint32_t kActivityRootExact = 0x1U;
inline constexpr std::uint32_t kActivityScenarioExact = 0x2U;
inline constexpr std::uint32_t kActivityContentExact = 0x4U;
inline constexpr std::uint32_t kActivityExtractionPresent = 0x8U;
inline constexpr std::uint32_t kActivityExactMask = kActivityRootExact | kActivityScenarioExact
                                                    | kActivityContentExact
                                                    | kActivityExtractionPresent;
/** Activity-binding flags preserve source presence and the completeness decision. */
inline constexpr std::uint32_t kActivityBindingFullSdkAcceptable = 0x1U;
inline constexpr std::uint32_t kActivityBindingHasInternalName = 0x2U;
inline constexpr std::uint32_t kActivityBindingHasMatchmakingConfig = 0x4U;
inline constexpr std::uint32_t kActivityBindingFlagMask = kActivityBindingFullSdkAcceptable
                                                          | kActivityBindingHasInternalName
                                                          | kActivityBindingHasMatchmakingConfig;
/** State rows expose only enabled and extraction-complete bits. */
inline constexpr std::uint32_t kStateFlagMask = 0x3U;
/** Slot-value decoding was independently verified for this row. */
inline constexpr std::uint32_t kSlotReaderVerified = 0x1U;
/** The package descriptor and reflected slot schemas joined exactly. */
inline constexpr std::uint32_t kSlotSchemaJoinExact = 0x2U;
/** A type-53 descriptor resolved an exact authored cue list and bounded cue count. */
inline constexpr std::uint32_t kSlotDialogueCuesExact = 0x4U;
/** Slot rows expose only reader, schema-join, and authored-dialogue facts. */
inline constexpr std::uint32_t kSlotFlagMask =
    kSlotReaderVerified | kSlotSchemaJoinExact | kSlotDialogueCuesExact;
/** Exact generated slot tuples for authored sequence and cinematic actions. */
inline constexpr std::uint32_t kObjectSlotType = 4U;
inline constexpr std::uint32_t kObjectComponentClass = 0x80809927U;
inline constexpr std::uint32_t kObjectSenseSchema = 0x8080992EU;
inline constexpr std::uint32_t kObjectAuthSchema = 0x8080992FU;
inline constexpr std::uint32_t kSequenceSlotType = 5U;
inline constexpr std::uint32_t kSequenceComponentClass = 0x80804F01U;
inline constexpr std::uint32_t kSequenceAuthSchema = 0x80804F04U;
inline constexpr std::uint32_t kCinematicSlotType = 6U;
inline constexpr std::uint32_t kCinematicComponentClass = 0x80804F06U;
inline constexpr std::uint32_t kCinematicAuthSchema = 0x80804F08U;
/** Exact generated slot tuple for the three-channel device Auth surface. */
inline constexpr std::uint32_t kDeviceSlotType = 23U;
inline constexpr std::uint32_t kDeviceComponentClass = 0x80804F45U;
inline constexpr std::uint32_t kDeviceSenseSchema = 0x80804F47U;
inline constexpr std::uint32_t kDeviceAuthSchema = 0x80804F48U;
/** Exact generated slot tuple for the occupancy-condition Auth surface. */
inline constexpr std::uint32_t kOccupancySlotType = 30U;
inline constexpr std::uint32_t kOccupancyComponentClass = 0x8080952FU;
inline constexpr std::uint32_t kOccupancySenseSchema = 0x80809531U;
inline constexpr std::uint32_t kOccupancyAuthSchema = 0x80809532U;
/** Authored three-lane HUD directive state. */
inline constexpr std::uint32_t kDirectiveSlotType = 68U;
inline constexpr std::uint32_t kDirectiveComponentClass = 0x80804F53U;
inline constexpr std::uint32_t kDirectiveAuthSchema = 0x80804F67U;
/** Exact generated slot tuple and descriptor field for authored dialogue. */
inline constexpr std::uint32_t kDialogueSlotType = 53U;
inline constexpr std::uint32_t kDialogueComponentClass = 0x80804F4BU;
inline constexpr std::uint32_t kDialogueAuthSchema = 0x80804F77U;
inline constexpr std::uint32_t kDialogueAuthoredListRelativeOffset = 0x58U;
inline constexpr std::uint32_t kDialogueAuthoredListClass = 0x80808D54U;
inline constexpr std::uint32_t kDialogueDefinitionArrayClass = 0x80808D18U;
inline constexpr std::uint32_t kDialogueGroupArrayClass = 0x80808D19U;
inline constexpr std::uint32_t kDialogueMaximumCueCount = 128U;
/** Inspect exposure is read-only and may be rendered by diagnostics. */
inline constexpr std::uint32_t kInspectExposure = 0x1U;
/** Panel-test exposure marks a candidate that still passes runtime gates. */
inline constexpr std::uint32_t kPanelTestExposure = 0x2U;
/** Script exposure marks a capability backed by a typed native mission intent. */
inline constexpr std::uint32_t kScriptExposure = 0x4U;
/** No other exposure bits are part of schema version one. */
inline constexpr std::uint32_t kExposureMask =
    kInspectExposure | kPanelTestExposure | kScriptExposure;
/** Signed relative offsets use one value that cannot name a package byte. */
inline constexpr std::int64_t kAbsentRelativeOffset = (-0x7FFFFFFFFFFFFFFFLL - 1);
/** Exact package classes retained by the actor RSAT projection. */
inline constexpr std::uint32_t kActorRsatDescriptorClass = 0x80809BB8U;
inline constexpr std::uint32_t kActorRsatSchemaClass = 0x80809BBBU;
inline constexpr std::uint32_t kActorRsatSchemaFieldClass = 0x80808852U;
/** Actor RSAT descriptor arrays start at this package-row offset. */
inline constexpr std::uint32_t kActorRsatDescriptorArrayOffset = 0x30U;
/** Actor definitions name their package-backed behavior configuration at this offset. */
inline constexpr std::uint32_t kActorBehaviorConfigRelativeOffset = 0x300U;
/** Physical package class of an exact actor behavior configuration reference. */
inline constexpr std::uint32_t kActorBehaviorConfigClass = 0x80809C36U;
/** Engine-semantics rows expose only facts tied to the pinned executable. */
enum class ActorSemanticProvenance : std::uint32_t {
    executableStatic = 1,
    packageField = 2,
    engineZeroDefault = 3,
    notPresent = 4,
};

enum class ActorCommandEffect : std::uint32_t {
    opaque = 0,
    setFaction = 1,
};

/** Row flag bits. `Exact` means every fact of that row came from the pinned executable. */
inline constexpr std::uint32_t kActorMessageSchemaExact = 0x1U;
inline constexpr std::uint32_t kActorCommandDefinitionExact = 0x1U;
inline constexpr std::uint32_t kActorBehaviorProfileExact = 0x1U;
inline constexpr std::uint32_t kSimulationEventDefinitionExact = 0x1U;
inline constexpr std::uint32_t kSimulationEventPrimaryAbsent = 0x2U;
inline constexpr std::uint32_t kSimulationEventSecondaryAbsent = 0x4U;
inline constexpr std::uint32_t kRuntimeSchemaExact = 0x1U;
inline constexpr std::uint32_t kRuntimeSchemaArrayRegion = 0x2U;
inline constexpr std::uint32_t kRuntimeFieldExact = 0x1U;
inline constexpr std::uint32_t kRuntimeFieldPresenceBit = 0x2U;
inline constexpr std::uint32_t kRuntimeFieldDynamicActorCommand = 0x4U;
inline constexpr std::uint32_t kRuntimeFieldNestedSchema = 0x8U;
inline constexpr std::uint32_t kRuntimeFieldDynamicArray = 0x10U;
inline constexpr std::uint32_t kRuntimeFieldCustomCodec = 0x20U;
inline constexpr std::uint32_t kRuntimeTypeDefinitionExact = 0x1U;
inline constexpr std::uint32_t kRuntimeTypeFixed = 0x2U;
inline constexpr std::uint32_t kRuntimeTypeParametric = 0x4U;
inline constexpr std::uint32_t kRuntimeTypeValueDependent = 0x8U;
inline constexpr std::uint32_t kRuntimeTypeNullable = 0x10U;
inline constexpr std::uint32_t kRuntimeTypeNested = 0x20U;
inline constexpr std::uint32_t kRuntimeTypeUnion = 0x40U;
inline constexpr std::uint32_t kRuntimeTypeResolved = 0x80U;
inline constexpr std::uint32_t kRuntimeTypeRemapped = 0x100U;
inline constexpr std::uint32_t kRuntimeTypeUnsupported = 0x200U;
inline constexpr std::uint32_t kRuntimeTypeObfuscated = 0x400U;
inline constexpr std::uint32_t kRuntimeTypeSelectorValidationRequired = 0x800U;

/** Executable reflection field codes used by the published runtime schemas. */
enum class RuntimeFieldType : std::uint32_t {
    nested = 1,
    boolean = 2,
    signed8 = 3,
    signed16 = 4,
    signed32 = 5,
    signed64 = 6,
    unsigned8 = 7,
    unsigned16 = 8,
    unsigned32 = 9,
    unsigned64 = 10,
    quantizedFloat = 11,
    vector3 = 13,
    compressedVector = 14,
    entityToken = 18,
    nullableTag = 22,
    dynamicSchema = 23,
    variant = 24,
    resolvedVariant = 25,
    custom28 = 28,
    actorCommand = 36,
};

/** Native reflection codec family required by one materialized schema. */
enum class RuntimeCodecFamily : std::uint32_t {
    activity = 1,
    sobjectModeZero = 2,
    sobjectModeOne = 4,
};
/** SObject RSAT and entity-type row flag bits, in the order the extraction sets them. */
inline constexpr std::uint32_t kSobjectRsatExact = 0x1U;
inline constexpr std::uint32_t kSobjectRsatDescriptorDynamicPresenceEligible = 0x1U;
inline constexpr std::uint32_t kEntityTypeDefinitionExact = 0x1U;
inline constexpr std::uint32_t kEntityTypeStockEmittable = 0x2U;
inline constexpr std::uint32_t kEntityTypeUpdateSupported = 0x4U;
inline constexpr std::uint32_t kEntityTypeUpdateUsesSobjectRsat = 0x8U;
inline constexpr std::uint32_t kSobjectRsatFieldBindingExact = 0x1U;
inline constexpr std::uint32_t kSobjectRsatFieldBindingHasRuntimeSchema = 0x2U;
/** RSAT schema field arrays start at this package-row offset. */
inline constexpr std::uint32_t kActorRsatSchemaFieldArrayOffset = 0x20U;
/** Descriptor flags retain only the exact dynamic-tail eligibility bit. */
inline constexpr std::uint32_t kRsatDescriptorDynamicPresenceEligible = 0x1U;
inline constexpr std::uint32_t kRsatDescriptorFlagMask = kRsatDescriptorDynamicPresenceEligible;
/** Schema flags retain eligibility and the typed-array shape. */
inline constexpr std::uint32_t kRsatSchemaDynamicPresenceEligible = 0x1U;
inline constexpr std::uint32_t kRsatSchemaTypedFieldArray = 0x2U;
inline constexpr std::uint32_t kRsatSchemaFlagMask =
    kRsatSchemaDynamicPresenceEligible | kRsatSchemaTypedFieldArray;
/** Runnable squads retain all six exact extraction facts. */
inline constexpr std::uint32_t kSquadSourceDescriptorExact = 0x1U;
inline constexpr std::uint32_t kSquadSpawnerRuleEdgeExact = 0x2U;
inline constexpr std::uint32_t kSquadScenarioOccurrenceExact = 0x4U;
inline constexpr std::uint32_t kSquadAllPointsExact = 0x8U;
inline constexpr std::uint32_t kSquadMemberCountValid = 0x10U;
inline constexpr std::uint32_t kSquadCandidateCountsInvariantComplete = 0x20U;
inline constexpr std::uint32_t kSquadFlagMask =
    kSquadSourceDescriptorExact | kSquadSpawnerRuleEdgeExact | kSquadScenarioOccurrenceExact
    | kSquadAllPointsExact | kSquadMemberCountValid | kSquadCandidateCountsInvariantComplete;
/** A squad runs only with every extraction fact present. */
inline constexpr std::uint32_t kSquadRunnableMask = kSquadFlagMask;
/** Member flags separate actor resolution from count-array completeness. */
inline constexpr std::uint32_t kSquadMemberActorClassExact = 0x1U;
inline constexpr std::uint32_t kSquadMemberCandidateCountsComplete = 0x2U;
inline constexpr std::uint32_t kSquadMemberCandidateCountsInvariant = 0x4U;
inline constexpr std::uint32_t kSquadMemberNoNullCandidates = 0x8U;
inline constexpr std::uint32_t kSquadMemberFlagMask = 0xFU;
inline constexpr std::uint32_t kSquadMemberInvariantReadyMask =
    kSquadMemberCandidateCountsComplete | kSquadMemberCandidateCountsInvariant
    | kSquadMemberNoNullCandidates;
/** Anchor rows carry only the exact placed-entry and position fact. */
inline constexpr std::uint32_t kSquadAnchorExact = 0x1U;
/** The placement came from the spawner's own point set, not from an object list. */
inline constexpr std::uint32_t kSquadAnchorInline = 0x2U;
/** The placement came from a container list or a type-4 descriptor, not the object's own list. */
inline constexpr std::uint32_t kSquadAnchorExternal = 0x4U;
inline constexpr std::uint32_t kSquadAnchorFlagMask =
    kSquadAnchorExact | kSquadAnchorInline | kSquadAnchorExternal;
/** Type-43 resource rows carry one exact package descriptor reference. */
inline constexpr std::uint32_t kAuthoredSceneResourceExact = 0x1U;
inline constexpr std::uint32_t kAuthoredSceneResourceFlagMask = kAuthoredSceneResourceExact;
inline constexpr std::uint32_t kAuthoredSceneSlotType = 43U;
inline constexpr std::uint32_t kAuthoredSceneComponentClass = 0x80806382U;
inline constexpr std::uint32_t kAuthoredSceneSenseSchema = 0x8080626AU;
inline constexpr std::uint32_t kAuthoredSceneAuthSchema = 0x8080626BU;
inline constexpr std::uint32_t kAuthoredSceneResourceRelativeOffset = 0x60U;
inline constexpr std::uint32_t kAuthoredSceneResourceClass = 0x80809C0FU;
/** Scene-to-squad rows retain one exact same-object package reference. */
inline constexpr std::uint32_t kAuthoredSceneSquadSameObjectExact = 0x1U;
/** A type-42 performance sensor names the same-object squad it drives at descriptor +0x58. */
inline constexpr std::uint32_t kAuthoredSceneSquadPerformanceTargetExact = 0x2U;
inline constexpr std::uint32_t kAuthoredSceneSquadFlagMask =
    kAuthoredSceneSquadSameObjectExact | kAuthoredSceneSquadPerformanceTargetExact;
/** The package slot type, classes and descriptor offset that name a performance sensor. */
inline constexpr std::uint32_t kPerformanceSlotType = 42U;
inline constexpr std::uint32_t kPerformanceComponentClass = 0x80809583U;
inline constexpr std::uint32_t kPerformanceAuthSchema = 0x80809586U;
inline constexpr std::uint32_t kPerformanceSquadReferenceRelativeOffset = 0x58U;
/** The actor definition that lists state-machine state names, and that group's name hash. */
inline constexpr std::uint32_t kActorStateMachineDefinitionClass = 0x8080815FU;
inline constexpr std::uint32_t kActorStateMachineGroupHash = 0xAFB11A12U;
inline constexpr std::uint32_t kActorStateNameExact = 0x1U;
inline constexpr std::uint32_t kActorStateNameFlagMask = kActorStateNameExact;
/** Sequence metadata is exact package data; names additionally require a verified FNV-1 match. */
inline constexpr std::uint32_t kActorSequenceExact = 1;
inline constexpr std::uint32_t kActorSequenceNameVerified = 2;
inline constexpr std::uint32_t kActorSequenceSymbolFromPath = 4;
inline constexpr std::uint32_t kActorSequenceEntryFlagMask = 7;
/** Native global and actor-local definition classes own these two action table fields. */
inline constexpr std::uint32_t kActorSequenceGlobalDefinitionClass = 0x8080816CU;
inline constexpr std::uint32_t kActorSequenceLocalDefinitionClass = 0x8080815FU;
inline constexpr std::uint32_t kActorSequenceGlobalArrayOffset = 152;
inline constexpr std::uint32_t kActorSequenceLocalArrayOffset = 16;
inline constexpr std::uint32_t kActorSequenceOwnerSourceClass = 0x808082ECU;
inline constexpr std::uint32_t kAuthoredSceneSquadBlockClassRelativeOffset = 0xA4U;
inline constexpr std::uint32_t kAuthoredSceneSquadBlockClass = 0x80806262U;
inline constexpr std::uint32_t kAuthoredSceneSquadReferenceRelativeOffset = 0xB0U;
/** Exact type-38 task edge to the authored type-3 objective component it mutates. */
inline constexpr std::uint32_t kTaskSlotType = 38U;
inline constexpr std::uint32_t kTaskComponentClass = 0x80807D87U;
inline constexpr std::uint32_t kTaskAuthSchema = 0x80807D89U;
inline constexpr std::uint32_t kTaskReferenceRelativeOffset = 0x58U;
inline constexpr std::uint32_t kTaskBitIndexRelativeOffset = 0x60U;
inline constexpr std::uint32_t kTaskTargetExact = 0x1U;
inline constexpr std::uint32_t kTaskTargetFlagMask = kTaskTargetExact;
inline constexpr std::uint32_t kObjectiveSlotType = 3U;
inline constexpr std::uint32_t kObjectiveComponentClass = 0x80808348U;
inline constexpr std::uint32_t kObjectiveSenseSchema = 0x80807F04U;
inline constexpr std::uint32_t kObjectiveAuthSchema = 0x80807F0CU;
/** Type-1 source slots must retain the exact reflected auth identity. */
inline constexpr std::uint32_t kSquadSlotType = 1U;
inline constexpr std::uint32_t kSquadAuthSchema = 0x80807EC9U;
inline constexpr std::uint32_t kSquadSenseSchema = 0x80807ECCU;
inline constexpr std::uint32_t kSquadComponentClass = 0x80809A3BU;
/** Four bits carry a nonempty list of at most fifteen authored members. */
inline constexpr std::uint32_t kSquadMinimumMemberCount = 1U;
inline constexpr std::uint32_t kSquadMaximumMemberCount = 15U;
/** Each member retains one authored count from all six candidate arrays. */
inline constexpr std::size_t kSquadCandidateCountLaneCount = 6;
/** Captured-estate message total, available only to regression fixtures. */
inline constexpr std::uint32_t kActivityMessageCount = 59U;
/** Message and field rows are data-only declarations, never generic write authority. */
inline constexpr std::uint32_t kActivityMessageDataOnly = 0x1U;
inline constexpr std::uint32_t kActivityMessageFieldPresenceBit = 0x1U;
inline constexpr std::uint32_t kActivityMessageFieldCoinedName = 0x2U;
inline constexpr std::uint32_t kActivityMessageFieldDocumentedRow = 0x4U;
inline constexpr std::uint32_t kActivityMessageFieldRepeatedBlock = 0x8U;
inline constexpr std::uint32_t kActivityMessageFieldDataOnly = 0x10U;
/**
 * Operator exposure grants. Neither bit means redacted, so a field a generator never reached shows
 * nothing. Both bits together are invalid and the validators refuse the row.
 */
inline constexpr std::uint32_t kActivityMessageFieldOperatorValue = 0x40U;
inline constexpr std::uint32_t kActivityMessageFieldProvisionalValue = 0x80U;
inline constexpr std::uint32_t kActivityMessageFieldExposureMask =
    kActivityMessageFieldOperatorValue | kActivityMessageFieldProvisionalValue;

/** Three closed exposure grades a decoded scalar can carry toward the operator sidecar. */
enum class ActivityMessageFieldExposure : std::uint32_t {
    redacted = 0,
    operatorValue = 1,
    provisionalValue = 2,
};

/** @return True when the row carries at most one exposure grant. */
[[nodiscard]] constexpr bool valid_field_exposure(std::uint32_t flags) noexcept {
    return (flags & kActivityMessageFieldExposureMask) != kActivityMessageFieldExposureMask;
}

/** @return The exposure a field row grants, defaulting to redacted. */
[[nodiscard]] constexpr ActivityMessageFieldExposure field_exposure(std::uint32_t flags) noexcept {
    if ((flags & kActivityMessageFieldOperatorValue) != 0) {
        return ActivityMessageFieldExposure::operatorValue;
    }
    if ((flags & kActivityMessageFieldProvisionalValue) != 0) {
        return ActivityMessageFieldExposure::provisionalValue;
    }
    return ActivityMessageFieldExposure::redacted;
}
/** Route rows are data-only declarations, never generic send authority. */
inline constexpr std::uint32_t kActivityCommunicationDataOnly = 0x1U;
/** Generated typed mission surfaces, the capacity a route's surface list is sized from. */
inline constexpr std::uint32_t kMissionSurfaceCount = 30U;
/** Absent signed wire metadata uses the only value outside every admitted field value. */
inline constexpr std::int64_t kAbsentSignedValue = (-0x7FFFFFFFFFFFFFFFLL - 1);

/** Fixed packed byte sizes make producer and consumer ABI drift fail at compile time. */
inline constexpr std::size_t kSectionSize = 16;
inline constexpr std::size_t kHeaderSize = 896;
inline constexpr std::size_t kStringRefSize = 8;
inline constexpr std::size_t kRangeSize = 8;
inline constexpr std::size_t kActivitySize = 124;
inline constexpr std::size_t kActivityBindingTagSize = 4;
inline constexpr std::size_t kActivityBindingLocatorSize = 16;
inline constexpr std::size_t kScenarioSize = 48;
inline constexpr std::size_t kBubbleSize = 40;
inline constexpr std::size_t kStateSize = 64;
inline constexpr std::size_t kObjectSize = 52;
inline constexpr std::size_t kOccurrenceSize = 56;
inline constexpr std::size_t kSlotSize = 80;
inline constexpr std::size_t kTextSize = 16;
inline constexpr std::size_t kCapabilitySize = 56;
inline constexpr std::size_t kGateSize = 48;
inline constexpr std::size_t kRefusalSize = 40;
inline constexpr std::size_t kActorClassSize = 68;
inline constexpr std::size_t kRsatDescriptorSize = 92;
inline constexpr std::size_t kRsatSchemaSize = 64;
inline constexpr std::size_t kRsatFieldSize = 40;
inline constexpr std::size_t kSquadSize = 52;
inline constexpr std::size_t kSquadMemberSize = 44;
inline constexpr std::size_t kSquadAnchorSize = 48;
inline constexpr std::size_t kAuthoredSceneResourceSize = 40;
inline constexpr std::size_t kAuthoredSceneSquadEdgeSize = 40;
inline constexpr std::size_t kTaskTargetSize = 44;
inline constexpr std::size_t kDialogueCueTextSize = 36;
inline constexpr std::size_t kDirectiveElementSize = 56;
inline constexpr std::size_t kBehaviorProgramSize = 28;
inline constexpr std::size_t kBehaviorInputSize = 36;
inline constexpr std::size_t kBehaviorChannelWriteSize = 16;
inline constexpr std::size_t kBehaviorOwnerSize = 32;
inline constexpr std::size_t kBehaviorActivityBindingSize = 32;
inline constexpr std::size_t kActorMessageSchemaSize = 48;
inline constexpr std::size_t kActorCommandDefinitionSize = 64;
inline constexpr std::size_t kActorBehaviorProfileSize = 32;
inline constexpr std::size_t kSimulationEventDefinitionSize = 56;
inline constexpr std::size_t kRuntimeSchemaSize = 48;
inline constexpr std::size_t kRuntimeFieldSize = 56;
inline constexpr std::size_t kRuntimeTypeDefinitionSize = 56;
inline constexpr std::size_t kSobjectRsatSize = 56;
inline constexpr std::size_t kSobjectRsatDescriptorSize = 72;
inline constexpr std::size_t kEntityTypeDefinitionSize = 56;
inline constexpr std::size_t kSobjectRsatFieldBindingSize = 40;
inline constexpr std::size_t kActorStateNameSize = 24;
inline constexpr std::size_t kActorSequenceTableSize = 24;
inline constexpr std::size_t kActorSequenceEntrySize = 64;
inline constexpr std::size_t kActorSequenceBindingSize = 32;
inline constexpr std::size_t kRsatDescriptorRawRowSize = 32;
inline constexpr std::size_t kRsatSchemaFieldRawRowSize = 40;

/** Expected packed field offsets pin producer-consumer order as well as total row size. */
namespace offset {
/** Every constant below is the current packed byte offset for its named field. */
inline constexpr std::size_t kSectionOffset = 0;
inline constexpr std::size_t kSectionCount = 8;
inline constexpr std::size_t kSectionStride = 12;
inline constexpr std::size_t kHeaderMagic = 0;
inline constexpr std::size_t kHeaderVersion = 8;
inline constexpr std::size_t kHeaderHeaderSize = 12;
inline constexpr std::size_t kHeaderFileSize = 16;
inline constexpr std::size_t kHeaderPayloadSha256 = 24;
inline constexpr std::size_t kHeaderSdkBuildSha256 = 56;
inline constexpr std::size_t kHeaderContentKeySha256 = 88;
inline constexpr std::size_t kHeaderLogicalIrSha256 = 120;
inline constexpr std::size_t kHeaderSectionCount = 152;
inline constexpr std::size_t kHeaderReserved = 156;
inline constexpr std::size_t kHeaderSections = 160;
inline constexpr std::size_t kStringRefOffset = 0;
inline constexpr std::size_t kStringRefLength = 4;
inline constexpr std::size_t kRangeFirst = 0;
inline constexpr std::size_t kRangeCount = 4;
inline constexpr std::size_t kActivityActivityIndex = 0;
inline constexpr std::size_t kActivityDefinitionHash = 4;
inline constexpr std::size_t kActivityId = 8;
inline constexpr std::size_t kActivityInternalName = 16;
inline constexpr std::size_t kActivityDisplayName = 24;
inline constexpr std::size_t kActivityScenarioIndex = 32;
inline constexpr std::size_t kActivityFlags = 36;
inline constexpr std::size_t kActivityAliases = 40;
inline constexpr std::size_t kActivityCapabilities = 48;
inline constexpr std::size_t kActivitySelectedActivityRootTag = 56;
inline constexpr std::size_t kActivitySelectedScenarioTag = 60;
inline constexpr std::size_t kActivityMatchmakingConfigTag = 64;
inline constexpr std::size_t kActivityJoinStatus = 68;
inline constexpr std::size_t kActivityBindingDisposition = 72;
inline constexpr std::size_t kActivityBindingReason = 76;
inline constexpr std::size_t kActivityBindingEvidenceBasis = 80;
inline constexpr std::size_t kActivityRunnableStatus = 84;
inline constexpr std::size_t kActivityBindingFlags = 88;
inline constexpr std::size_t kActivityRootCandidateTags = 92;
inline constexpr std::size_t kActivityScenarioNameCandidateTags = 100;
inline constexpr std::size_t kActivityEvidenceRootTags = 108;
inline constexpr std::size_t kActivityBindingLocators = 116;
inline constexpr std::size_t kActivityBindingTagTag = 0;
inline constexpr std::size_t kActivityBindingLocatorTag = 0;
inline constexpr std::size_t kActivityBindingLocatorReserved = 4;
inline constexpr std::size_t kActivityBindingLocatorOffset = 8;
inline constexpr std::size_t kScenarioTag = 0;
inline constexpr std::size_t kScenarioReserved = 4;
inline constexpr std::size_t kScenarioId = 8;
inline constexpr std::size_t kScenarioName = 16;
inline constexpr std::size_t kScenarioBubbles = 24;
inline constexpr std::size_t kScenarioStates = 32;
inline constexpr std::size_t kScenarioOccurrences = 40;
inline constexpr std::size_t kBubbleId = 0;
inline constexpr std::size_t kBubbleName = 8;
inline constexpr std::size_t kBubbleScenarioIndex = 16;
inline constexpr std::size_t kBubbleBubbleOrdinal = 20;
inline constexpr std::size_t kBubbleNameHash = 24;
inline constexpr std::size_t kBubbleReserved = 28;
inline constexpr std::size_t kBubbleStates = 32;
inline constexpr std::size_t kStateId = 0;
inline constexpr std::size_t kStateEntryId = 8;
inline constexpr std::size_t kStateRegistryId = 16;
inline constexpr std::size_t kStateScenarioIndex = 24;
inline constexpr std::size_t kStateBubbleIndex = 28;
inline constexpr std::size_t kStateStateOrdinal = 32;
inline constexpr std::size_t kStateEntryIndex = 36;
inline constexpr std::size_t kStateSliceSetIndex = 40;
inline constexpr std::size_t kStateMapBubbleIndex = 44;
inline constexpr std::size_t kStateStateHash = 48;
inline constexpr std::size_t kStatePublicValue = 52;
inline constexpr std::size_t kStateFlags = 56;
inline constexpr std::size_t kStateRegistryTag = 60;
inline constexpr std::size_t kObjectId = 0;
inline constexpr std::size_t kObjectObjectTag = 8;
inline constexpr std::size_t kObjectObjectKey = 12;
inline constexpr std::size_t kObjectSlots = 16;
inline constexpr std::size_t kObjectConfigCount = 24;
inline constexpr std::size_t kObjectDescriptorCount = 28;
inline constexpr std::size_t kObjectPlacedSubblockCount = 32;
inline constexpr std::size_t kObjectPlacedLeafCount = 36;
inline constexpr std::size_t kObjectPlacedHopCount = 40;
inline constexpr std::size_t kObjectBareTargetCount = 44;
inline constexpr std::size_t kOccurrenceId = 0;
inline constexpr std::size_t kOccurrenceContextRegistryKey = 8;
inline constexpr std::size_t kOccurrenceRegistryId = 16;
inline constexpr std::size_t kOccurrenceEntryId = 24;
inline constexpr std::size_t kOccurrenceScenarioIndex = 32;
inline constexpr std::size_t kOccurrenceBubbleIndex = 36;
inline constexpr std::size_t kOccurrenceStateIndex = 40;
inline constexpr std::size_t kOccurrenceObjectIndex = 44;
inline constexpr std::size_t kOccurrenceRegistryField = 48;
inline constexpr std::size_t kOccurrenceObjectOrdinal = 52;
inline constexpr std::size_t kSlotId = 0;
inline constexpr std::size_t kSlotName = 8;
inline constexpr std::size_t kSlotSenseSchemaId = 16;
inline constexpr std::size_t kSlotAuthSchemaId = 24;
inline constexpr std::size_t kSlotObjectIndex = 32;
inline constexpr std::size_t kSlotSlotIndex = 36;
inline constexpr std::size_t kSlotSlotType = 40;
inline constexpr std::size_t kSlotComponentClass = 44;
inline constexpr std::size_t kSlotSenseSchema = 48;
inline constexpr std::size_t kSlotAuthSchema = 52;
inline constexpr std::size_t kSlotFlags = 56;
inline constexpr std::size_t kSlotReserved = 60;
inline constexpr std::size_t kSlotAliases = 64;
inline constexpr std::size_t kSlotCapabilities = 72;
inline constexpr std::size_t kTextValue = 0;
inline constexpr std::size_t kTextKind = 8;
inline constexpr std::size_t kTextReserved = 12;
inline constexpr std::size_t kCapabilityId = 0;
inline constexpr std::size_t kCapabilityOperation = 8;
inline constexpr std::size_t kCapabilityValueSchemaId = 16;
inline constexpr std::size_t kCapabilitySubjectKind = 24;
inline constexpr std::size_t kCapabilitySubjectIndex = 28;
inline constexpr std::size_t kCapabilityExposureFlags = 32;
inline constexpr std::size_t kCapabilityCandidateExposureFlags = 36;
inline constexpr std::size_t kCapabilityGates = 40;
inline constexpr std::size_t kCapabilityRefusals = 48;
inline constexpr std::size_t kGateGate = 0;
inline constexpr std::size_t kGateStatus = 8;
inline constexpr std::size_t kGateReasonCode = 16;
inline constexpr std::size_t kGateRequired = 24;
inline constexpr std::size_t kGateObserved = 32;
inline constexpr std::size_t kGateWouldConfirm = 40;
inline constexpr std::size_t kRefusalId = 0;
inline constexpr std::size_t kRefusalExposure = 8;
inline constexpr std::size_t kRefusalStatus = 16;
inline constexpr std::size_t kRefusalReasonCodes = 24;
inline constexpr std::size_t kRefusalCapabilityIndex = 32;
inline constexpr std::size_t kRefusalReserved = 36;
inline constexpr std::size_t kActorClassId = 0;
inline constexpr std::size_t kActorClassDefinitionTag = 8;
inline constexpr std::size_t kActorClassNameHash = 12;
inline constexpr std::size_t kActorClassRsatTag = 16;
inline constexpr std::size_t kActorClassRsatReverseDefinitionTag = 20;
inline constexpr std::size_t kActorClassObjectType = 24;
inline constexpr std::size_t kActorClassDescriptorArrayOffset = 28;
inline constexpr std::size_t kActorClassDescriptorArrayRelative = 32;
inline constexpr std::size_t kActorClassDescriptorArrayHeaderOffset = 40;
inline constexpr std::size_t kActorClassDescriptorArrayDataOffset = 44;
inline constexpr std::size_t kActorClassDescriptorElementClass = 48;
inline constexpr std::size_t kActorClassDescriptors = 52;
inline constexpr std::size_t kActorClassDynamicPresenceTailCount = 60;
inline constexpr std::size_t kActorClassReserved = 64;
inline constexpr std::size_t kRsatDescriptorId = 0;
inline constexpr std::size_t kRsatDescriptorActorClassIndex = 8;
inline constexpr std::size_t kRsatDescriptorRsatTag = 12;
inline constexpr std::size_t kRsatDescriptorOrdinal = 16;
inline constexpr std::size_t kRsatDescriptorOffset = 20;
inline constexpr std::size_t kRsatDescriptorElementClass = 24;
inline constexpr std::size_t kRsatDescriptorComponentTag = 28;
inline constexpr std::size_t kRsatDescriptorSchemaIndex = 32;
inline constexpr std::size_t kRsatDescriptorSchemaTag = 36;
inline constexpr std::size_t kRsatDescriptorSchemaFieldCount = 40;
inline constexpr std::size_t kRsatDescriptorSchemaFirstFieldRuntimeGate = 44;
inline constexpr std::size_t kRsatDescriptorSchemaFirstFieldRawU32At10 = 48;
inline constexpr std::size_t kRsatDescriptorFlags = 52;
inline constexpr std::size_t kRsatDescriptorDynamicPresenceTailOrdinal = 56;
inline constexpr std::size_t kRsatDescriptorRawRow = 60;
inline constexpr std::size_t kRsatSchemaId = 0;
inline constexpr std::size_t kRsatSchemaTag = 8;
inline constexpr std::size_t kRsatSchemaClass = 12;
inline constexpr std::size_t kRsatSchemaFieldCount = 16;
inline constexpr std::size_t kRsatSchemaFieldArrayOffset = 20;
inline constexpr std::size_t kRsatSchemaFieldArrayRelative = 24;
inline constexpr std::size_t kRsatSchemaFieldArrayHeaderOffset = 32;
inline constexpr std::size_t kRsatSchemaFieldArrayDataOffset = 36;
inline constexpr std::size_t kRsatSchemaFieldElementClass = 40;
inline constexpr std::size_t kRsatSchemaFirstFieldRuntimeGate = 44;
inline constexpr std::size_t kRsatSchemaFirstFieldRawU32At10 = 48;
inline constexpr std::size_t kRsatSchemaFlags = 52;
inline constexpr std::size_t kRsatSchemaFields = 56;
inline constexpr std::size_t kRsatFieldRawRow = 0;
inline constexpr std::size_t kSquadId = 0;
inline constexpr std::size_t kSquadScenarioIndex = 8;
inline constexpr std::size_t kSquadObjectIndex = 12;
inline constexpr std::size_t kSquadSlotIndex = 16;
inline constexpr std::size_t kSquadSpawnerConfigTag = 20;
inline constexpr std::size_t kSquadSpawnRuleConfigTag = 24;
inline constexpr std::size_t kSquadFlags = 28;
inline constexpr std::size_t kSquadOccurrenceIndex = 32;
inline constexpr std::size_t kSquadMembers = 36;
inline constexpr std::size_t kSquadAnchors = 44;
inline constexpr std::size_t kSquadMemberId = 0;
inline constexpr std::size_t kSquadMemberSquadIndex = 8;
inline constexpr std::size_t kSquadMemberMemberOrdinal = 12;
inline constexpr std::size_t kSquadMemberMemberKey = 16;
inline constexpr std::size_t kSquadMemberActorClassIndex = 20;
inline constexpr std::size_t kSquadMemberFlags = 24;
inline constexpr std::size_t kSquadMemberCandidateCounts = 28;
inline constexpr std::size_t kSquadMemberDefaultCount = 40;
inline constexpr std::size_t kSquadAnchorId = 0;
inline constexpr std::size_t kSquadAnchorSquadIndex = 8;
inline constexpr std::size_t kSquadAnchorPointOrdinal = 12;
inline constexpr std::size_t kSquadAnchorObjectListTag = 16;
inline constexpr std::size_t kSquadAnchorPlacementOrdinal = 20;
inline constexpr std::size_t kSquadAnchorFlags = 24;
inline constexpr std::size_t kSquadAnchorPlacedEntryIdentity = 28;
inline constexpr std::size_t kSquadAnchorPositionBits = 36;
inline constexpr std::size_t kAuthoredSceneResourceId = 0;
inline constexpr std::size_t kAuthoredSceneResourceSlotIndex = 8;
inline constexpr std::size_t kAuthoredSceneResourceConfigTag = 12;
inline constexpr std::size_t kAuthoredSceneResourceDescriptorOffset = 16;
inline constexpr std::size_t kAuthoredSceneResourceFieldOffset = 20;
inline constexpr std::size_t kAuthoredSceneResourceTag = 24;
inline constexpr std::size_t kAuthoredSceneResourceClass = 28;
inline constexpr std::size_t kAuthoredSceneResourceFlags = 32;
inline constexpr std::size_t kAuthoredSceneResourceReserved = 36;
inline constexpr std::size_t kAuthoredSceneSquadEdgeId = 0;
inline constexpr std::size_t kAuthoredSceneSquadEdgeSceneSlotIndex = 8;
inline constexpr std::size_t kAuthoredSceneSquadEdgeSquadSlotIndex = 12;
inline constexpr std::size_t kAuthoredSceneSquadEdgeConfigTag = 16;
inline constexpr std::size_t kAuthoredSceneSquadEdgeDescriptorOffset = 20;
inline constexpr std::size_t kAuthoredSceneSquadEdgeReferenceFieldOffset = 24;
inline constexpr std::size_t kAuthoredSceneSquadEdgeTargetObjectKey = 28;
inline constexpr std::size_t kAuthoredSceneSquadEdgeFlags = 32;
inline constexpr std::size_t kAuthoredSceneSquadEdgeReserved = 36;
inline constexpr std::size_t kTaskTargetId = 0;
inline constexpr std::size_t kTaskTargetTaskSlotIndex = 8;
inline constexpr std::size_t kTaskTargetObjectiveSlotIndex = 12;
inline constexpr std::size_t kTaskTargetConfigTag = 16;
inline constexpr std::size_t kTaskTargetDescriptorOffset = 20;
inline constexpr std::size_t kTaskTargetReferenceFieldOffset = 24;
inline constexpr std::size_t kTaskTargetTargetObjectKey = 28;
inline constexpr std::size_t kTaskTargetBitIndex = 32;
inline constexpr std::size_t kTaskTargetFlags = 36;
inline constexpr std::size_t kTaskTargetReserved = 40;
inline constexpr std::size_t kActorStateNameActorClassIndex = 0;
inline constexpr std::size_t kActorStateNameDefinitionTag = 4;
inline constexpr std::size_t kActorStateNameGroupHash = 8;
inline constexpr std::size_t kActorStateNameNameHash = 12;
inline constexpr std::size_t kActorStateNameOrdinal = 16;
inline constexpr std::size_t kActorStateNameFlags = 20;
} // namespace offset

/** Sections have a fixed order so every row range has one target domain. */
enum class SectionIndex : std::uint32_t {
    strings,
    activities,
    scenarios,
    bubbles,
    states,
    objects,
    occurrences,
    slots,
    texts,
    capabilities,
    gates,
    refusals,
    actorClasses,
    rsatDescriptors,
    rsatSchemas,
    rsatFields,
    squads,
    squadMembers,
    squadAnchors,
    authoredSceneResources,
    authoredSceneSquadEdges,
    taskTargets,
    dialogueCueTexts,
    directiveElements,
    activityBindingTags,
    activityBindingLocators,
    behaviorPrograms,
    behaviorInputs,
    behaviorChannelWrites,
    behaviorOwners,
    behaviorActivityBindings,
    actorMessageSchemas,
    actorCommandDefinitions,
    actorBehaviorProfiles,
    simulationEventDefinitions,
    runtimeSchemas,
    runtimeFields,
    sobjectRsats,
    sobjectRsatDescriptors,
    entityTypeDefinitions,
    sobjectRsatFieldBindings,
    runtimeTypeDefinitions,
    actorStateNames,
    actorSequenceTables,
    actorSequenceEntries,
    actorSequenceBindings,
};

static_assert(static_cast<std::uint32_t>(SectionIndex::actorSequenceBindings) + 1 == kSectionCount);

/** Exact activity-name/root join result retained for every activity row. */
enum class ActivityJoinStatus : std::uint32_t {
    exact = 0,
    liveNameMissing = 1,
    sourceNameMissing = 2,
    liveNameAmbiguous = 3,
};

/** Exhaustive fixed-scenario applicability result for one activity definition. */
enum class ActivityBindingDisposition : std::uint32_t {
    fixedScenario = 0,
    namedDefinitionUnavailable = 1,
    noDirectFixedActivityName = 2,
    unresolvedRunnable = 3,
};

/** Stable explanation for the selected activity-binding disposition. */
enum class ActivityBindingReason : std::uint32_t {
    exactActivityRootScenarioEdge = 0,
    installedRouteAbsent = 1,
    noDirectFixedActivityName = 2,
    activityRootNameAmbiguous = 3,
    activityRootEdgeMissing = 4,
};

/** Source evidence domain that supports an activity-binding classification. */
enum class ActivityBindingEvidenceBasis : std::uint32_t {
    effectiveActivityRootNamePlusPayloadScenarioEdge = 0,
    effectiveActivityAndScenarioRootNameCensus = 1,
    activityRecordInternalNameEmpty = 2,
    effectiveActivityRootNameCensus = 3,
};

/** Runtime interpretation retained independently from fixed-scenario applicability. */
enum class ActivityRunnableStatus : std::uint32_t {
    fixedScenarioBound = 0,
    unavailableInInstalledEstate = 1,
    fixedScenarioNotApplicable = 2,
    unresolved = 3,
};

/** Text rows keep aliases and refusal reasons separate without copying strings. */
enum class TextKind : std::uint32_t {
    internalAlias = 1,
    displayAlias = 2,
    slotAlias = 3,
    refusalReason = 4,
};

/** A capability subject is one activity, object slot, or catalog-global host API. */
enum class SubjectKind : std::uint32_t {
    activity = 1,
    slot = 2,
    hostApi = 3,
};

/** Direction values are copied from the checked activity-message registry. */
enum class ActivityMessageDirection : std::uint32_t {
    remoteToClient = 1,
    clientToRemote = 2,
    bidirectional = 3,
    clientToRemoteSpecial = 4,
    nameOnly = 5,
};

/** Coverage values distinguish exact wire grammars from explicit partial or name-only rows. */
enum class ActivityMessageCoverage : std::uint32_t {
    fixedWireExact = 1,
    customWireExact = 2,
    partialDynamicBody = 3,
    variableWire = 4,
    serviceConversion = 5,
    nameOnly = 6,
};

/** Activity messages either start directly or carry the proved delta-root bit. */
enum class ActivityMessageCallForm : std::uint32_t {
    direct = 1,
    deltaRootBit = 2,
};

/** Definition provenance separates absent, authored, and executable-graph rows. */
enum class ActivityMessageDefinitionState : std::uint32_t {
    none = 1,
    authored = 2,
    graph = 3,
};

/** One field comes from the executable definition graph or a checked authored grammar. */
enum class ActivityMessageFieldSource : std::uint32_t {
    graph = 1,
    authored = 2,
};

/** Field names retain their evidence grade rather than implying semantic certainty. */
enum class ActivityMessageFieldConfidence : std::uint32_t {
    verified = 1,
    assumed = 2,
    unnamed = 3,
};

#pragma pack(push, 1)

/** One section points at fixed-stride rows inside the mapped file. */
struct Section final {
    std::uint64_t offset{};
    std::uint32_t count{};
    std::uint32_t stride{};
};

/** The header binds the whole payload to one generated SDK identity. */
struct Header final {
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint64_t fileSize{};
    std::array<std::byte, 32> payloadSha256{};
    std::array<std::byte, 32> sdkBuildSha256{};
    std::array<std::byte, 32> contentKeySha256{};
    std::array<std::byte, 32> logicalIrSha256{};
    std::uint32_t sectionCount{};
    std::uint32_t reserved{};
    std::array<Section, kSectionCount> sections{};
};

/** String offsets are relative to the byte-string section. */
struct StringRef final {
    std::uint32_t offset{};
    std::uint32_t length{};
};

/** Row ranges are relative to the one section implied by their parent field. */
struct Range final {
    std::uint32_t first{};
    std::uint32_t count{};
};

/** One investment activity and its exact scenario binding. */
struct Activity final {
    std::uint32_t activityIndex{};
    std::uint32_t definitionHash{};
    StringRef id{};
    StringRef internalName{};
    StringRef displayName{};
    std::uint32_t scenarioIndex{};
    std::uint32_t flags{};
    Range aliases{};
    Range capabilities{};
    std::uint32_t selectedActivityRootTag{kAbsentIndex};
    std::uint32_t selectedScenarioTag{kAbsentIndex};
    std::uint32_t matchmakingConfigTag{kAbsentIndex};
    std::uint32_t joinStatus{};
    std::uint32_t bindingDisposition{};
    std::uint32_t bindingReason{};
    std::uint32_t bindingEvidenceBasis{};
    std::uint32_t runnableStatus{};
    std::uint32_t bindingFlags{};
    Range activityRootCandidateTags{};
    Range scenarioNameCandidateTags{};
    Range evidenceRootTags{};
    Range bindingLocators{};
};

/** One canonical package tag in an activity-owned candidate or evidence range. */
struct ActivityBindingTag final {
    std::uint32_t tag{};
};

/** One exact package payload location supporting an activity classification. */
struct ActivityBindingLocator final {
    std::uint32_t tag{};
    std::uint32_t reserved{};
    std::uint64_t offset{};
};

/** One scenario owns contiguous bubble, state, and occurrence ranges. */
struct Scenario final {
    std::uint32_t tag{};
    std::uint32_t reserved{};
    StringRef id{};
    StringRef name{};
    Range bubbles{};
    Range states{};
    Range occurrences{};
};

/** One scenario bubble owns a contiguous subset of that scenario's states. */
struct Bubble final {
    StringRef id{};
    StringRef name{};
    std::uint32_t scenarioIndex{};
    std::uint32_t bubbleOrdinal{};
    std::uint32_t nameHash{};
    std::uint32_t reserved{};
    Range states{};
};

/** One scenario state carries its package entry and registry identities. */
struct State final {
    StringRef id{};
    StringRef entryId{};
    StringRef registryId{};
    std::uint32_t scenarioIndex{};
    std::uint32_t bubbleIndex{};
    std::uint32_t stateOrdinal{};
    /** Exact index of this authored state entry in its owning scenario table. */
    std::uint32_t entryIndex{};
    /** Exact package slice-set index selected by this state; not a membership region. */
    std::uint32_t sliceSetIndex{};
    std::uint32_t mapBubbleIndex{};
    std::uint32_t stateHash{};
    std::uint32_t publicValue{};
    std::uint32_t flags{};
    std::uint32_t registryTag{};
};

/** One object definition owns its reusable slot definitions. */
struct Object final {
    StringRef id{};
    std::uint32_t objectTag{};
    std::uint32_t objectKey{};
    Range slots{};
    std::uint32_t configCount{};
    std::uint32_t descriptorCount{};
    std::uint32_t placedSubblockCount{};
    std::uint32_t placedLeafCount{};
    std::uint32_t placedHopCount{};
    std::uint32_t bareTargetCount{};
    /** Authored placements the game replicates. Zero with placements means client-built content. */
    std::uint32_t replicatedPlacementCount{};
};

/** One scenario-local occurrence binds an object to one registry context. */
struct Occurrence final {
    StringRef id{};
    StringRef contextRegistryKey{};
    StringRef registryId{};
    StringRef entryId{};
    std::uint32_t scenarioIndex{};
    std::uint32_t bubbleIndex{};
    std::uint32_t stateIndex{};
    std::uint32_t objectIndex{};
    std::uint32_t registryField{};
    std::uint32_t objectOrdinal{};
};

/** One object slot carries inspectable schema identity and capability ranges. */
struct Slot final {
    StringRef id{};
    StringRef name{};
    StringRef senseSchemaId{};
    StringRef authSchemaId{};
    std::uint32_t objectIndex{};
    std::uint32_t slotIndex{};
    std::uint32_t slotType{};
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    std::uint32_t flags{};
    /** Authored dialogue cue count when kSlotDialogueCuesExact is set; zero otherwise. */
    std::uint32_t reserved{};
    Range aliases{};
    Range capabilities{};
};

/** One typed string row is owned by an alias or refusal range. */
struct Text final {
    StringRef value{};
    std::uint32_t kind{};
    std::uint32_t reserved{};
};

/** One inspectable or panel-candidate operation carries all of its gates. */
struct Capability final {
    StringRef id{};
    StringRef operation{};
    StringRef valueSchemaId{};
    std::uint32_t subjectKind{};
    std::uint32_t subjectIndex{};
    std::uint32_t exposureFlags{};
    std::uint32_t candidateExposureFlags{};
    Range gates{};
    Range refusals{};
};

/** One capability gate records the required, observed, and refusal state. */
struct Gate final {
    StringRef gate{};
    StringRef status{};
    StringRef reasonCode{};
    StringRef required{};
    StringRef observed{};
    StringRef wouldConfirm{};
};

/** One refused exposure owns stable reason-code text rows. */
struct Refusal final {
    StringRef id{};
    StringRef exposure{};
    StringRef status{};
    Range reasonCodes{};
    std::uint32_t capabilityIndex{};
    std::uint32_t reserved{};
};

/** One actor definition owns its exact package RSAT descriptor sequence. */
struct ActorClass final {
    StringRef id{};
    std::uint32_t definitionTag{};
    std::uint32_t nameHash{};
    std::uint32_t rsatTag{};
    std::uint32_t rsatReverseDefinitionTag{};
    std::uint32_t objectType{};
    std::uint32_t descriptorArrayOffset{};
    std::int64_t descriptorArrayRelative{};
    std::uint32_t descriptorArrayHeaderOffset{};
    std::uint32_t descriptorArrayDataOffset{};
    std::uint32_t descriptorElementClass{};
    Range descriptors{};
    std::uint32_t dynamicPresenceTailCount{};
    std::array<std::int8_t, 4> authoredSpawnProfile{};
};

/** One ordered RSAT descriptor retains the complete 32-byte package row. */
struct RsatDescriptor final {
    StringRef id{};
    std::uint32_t actorClassIndex{};
    std::uint32_t rsatTag{};
    std::uint32_t descriptorOrdinal{};
    std::uint32_t descriptorOffset{};
    std::uint32_t descriptorElementClass{};
    std::uint32_t componentTag{};
    std::uint32_t schemaIndex{};
    std::uint32_t schemaTag{};
    std::uint32_t schemaFieldCount{};
    std::uint32_t schemaFirstFieldRuntimeGate{};
    std::uint32_t schemaFirstFieldRawU32At10{};
    std::uint32_t flags{};
    std::uint32_t dynamicPresenceTailOrdinal{};
    std::array<std::byte, kRsatDescriptorRawRowSize> rawRow{};
};

/** One RSAT schema owns its exact ordered 40-byte field rows. */
struct RsatSchema final {
    StringRef id{};
    std::uint32_t schemaTag{};
    std::uint32_t schemaClass{};
    std::uint32_t fieldCount{};
    std::uint32_t fieldArrayOffset{};
    std::int64_t fieldArrayRelative{};
    std::uint32_t fieldArrayHeaderOffset{};
    std::uint32_t fieldArrayDataOffset{};
    std::uint32_t fieldElementClass{};
    std::uint32_t firstFieldRuntimeGate{};
    std::uint32_t firstFieldRawU32At10{};
    std::uint32_t flags{};
    Range fields{};
};

/** One RSAT field is the unmodified 40-byte package row. */
struct RsatField final {
    std::array<std::byte, kRsatSchemaFieldRawRowSize> rawRow{};
};

/** One scenario-local type-1 source owns its authored members and exact anchors. */
struct Squad final {
    StringRef id{};
    std::uint32_t scenarioIndex{};
    std::uint32_t objectIndex{};
    std::uint32_t slotIndex{};
    std::uint32_t spawnerConfigTag{};
    std::uint32_t spawnRuleConfigTag{};
    std::uint32_t flags{};
    std::uint32_t occurrenceIndex{};
    Range members{};
    Range anchors{};
};

/** One authored squad member retains six candidate-array counts and its invariant default. */
struct SquadMember final {
    StringRef id{};
    std::uint32_t squadIndex{};
    std::uint32_t memberOrdinal{};
    std::uint32_t memberKey{};
    std::uint32_t actorClassIndex{kAbsentIndex};
    std::uint32_t flags{};
    std::array<std::uint16_t, kSquadCandidateCountLaneCount> candidateCounts{};
    std::int32_t defaultCount{-1};
};

/** One exact authored anchor retains its placed-entry identity and raw position bits. */
struct SquadAnchor final {
    StringRef id{};
    std::uint32_t squadIndex{};
    std::uint32_t pointOrdinal{};
    std::uint32_t objectListTag{};
    std::uint32_t placementOrdinal{};
    std::uint32_t flags{};
    std::uint64_t placedEntryIdentity{};
    std::array<std::uint32_t, 3> positionBits{};
};

/** One row retains a descriptor-relative type-43 package resource tag. */
struct AuthoredSceneResource final {
    StringRef id{};
    std::uint32_t slotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t resourceFieldOffset{};
    std::uint32_t resourceTag{};
    std::uint32_t resourceClass{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One type-43 slot retains an exact same-object reference to one type-1 squad slot. */
struct AuthoredSceneSquadEdge final {
    StringRef id{};
    std::uint32_t sceneSlotIndex{};
    std::uint32_t squadSlotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t referenceFieldOffset{};
    std::uint32_t targetObjectKey{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One task sensor names the exact objective bit its changed generation clears. */
struct TaskTarget final {
    StringRef id{};
    std::uint32_t taskSlotIndex{};
    std::uint32_t objectiveSlotIndex{};
    std::uint32_t configTag{};
    std::uint32_t descriptorOffset{};
    std::uint32_t referenceFieldOffset{};
    std::uint32_t targetObjectKey{};
    std::uint32_t bitIndex{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One localized variant belongs to one exact authored type-53 cue definition. */
struct DialogueCueText final {
    StringRef id{};
    StringRef text{};
    std::uint32_t slotIndex{};
    std::uint32_t cueIndex{};
    std::uint32_t definitionHash{};
    std::uint32_t containerTag{};
    std::uint32_t stringHash{};
};

/** One bounded authored type-68 HUD element with its exact title and description fields. */
struct DirectiveElement final {
    StringRef id{};
    StringRef title{};
    StringRef description{};
    std::uint32_t slotIndex{};
    std::uint32_t nameHash{};
    std::int32_t elementIndex{};
    std::uint32_t elementCount{};
    std::uint32_t titleContainerTag{};
    std::uint32_t titleStringHash{};
    std::uint32_t descriptionContainerTag{};
    std::uint32_t descriptionStringHash{};
};

/** One installed compiled behavior root and its complete local channel-edge ranges. */
struct BehaviorProgram final {
    std::uint32_t rootTag{};
    Range inputs{};
    Range channelWrites{};
    std::uint32_t nodeCount{};
    std::uint32_t expressionCount{};
};

/** One exact compiled expression read from an object-local float4 channel. */
struct BehaviorInput final {
    std::uint32_t programIndex{};
    std::uint32_t nodeOffset{};
    std::uint32_t expressionOffset{};
    std::uint32_t channelHash{};
    std::uint64_t inputOrMode{};
    std::int32_t nativeOverride{};
    std::uint32_t activeField{};
    std::uint8_t selector{};
    std::uint8_t role{};
    std::uint16_t reserved{};
};

/** One compiled storage action writing an object-local float4 channel. */
struct BehaviorChannelWrite final {
    std::uint32_t programIndex{};
    std::uint32_t nodeOffset{};
    std::uint32_t channelHash{};
    std::uint32_t reserved{};
};

enum class BehaviorSubmissionKind : std::uint32_t {
    activeNative,
    passive,
    unresolved,
};

/** One exact behavior root reference owned by an activity-reached actor class. */
struct BehaviorOwner final {
    std::uint32_t programIndex{};
    std::uint32_t actorClassIndex{};
    std::uint32_t configTag{};
    std::uint32_t configFieldOffset{};
    std::uint32_t buildOrdinal{};
    std::uint32_t descriptorOrdinal{kAbsentIndex};
    std::uint32_t submitterSubtype{};
    BehaviorSubmissionKind submissionKind{BehaviorSubmissionKind::unresolved};
};

/** One exact behavior owner path through a squad member and scenario occurrence. */
struct BehaviorActivityBinding final {
    std::uint32_t ownerIndex{};
    std::uint32_t squadIndex{};
    std::uint32_t squadMemberIndex{};
    std::uint32_t scenarioIndex{};
    std::uint32_t occurrenceIndex{};
    std::uint32_t stateIndex{};
    std::uint32_t objectIndex{};
    std::uint32_t reserved{};
};

/** One executable-derived SObject message schema that carries actor commands. */
struct ActorMessageSchema final {
    StringRef name{};
    std::uint32_t definitionHandle{};
    std::uint32_t durableKey{};
    std::uint32_t ownerClass{};
    std::uint32_t handlerSlot{};
    std::uint32_t bodyType{};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::executableStatic};
    Range commands{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One executable-derived actor command with its typed payload and faction values. */
struct ActorCommandDefinition final {
    StringRef name{};
    StringRef factionNoneName{};
    StringRef factionRemovedName{};
    StringRef factionHostileToAllName{};
    std::uint32_t selector{};
    std::uint32_t payloadHandle{};
    ActorCommandEffect effect{ActorCommandEffect::opaque};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::executableStatic};
    std::int32_t factionNone{};
    std::int32_t factionRemoved{};
    std::int32_t factionHostileToAll{};
    std::uint32_t flags{};
};

/** One actor joins its package behavior config to the engine's initial faction rule. */
struct ActorBehaviorProfile final {
    std::uint32_t actorClassIndex{};
    std::uint32_t behaviorConfigTag{};
    std::uint32_t behaviorConfigClass{};
    std::uint32_t behaviorConfigOffset{};
    std::int32_t defaultFaction{};
    ActorSemanticProvenance behaviorProvenance{ActorSemanticProvenance::packageField};
    ActorSemanticProvenance factionProvenance{ActorSemanticProvenance::engineZeroDefault};
    std::uint32_t flags{};
};

/** One executable-derived lane-0 simulation event definition. */
struct SimulationEventDefinition final {
    StringRef name{};
    std::uint32_t eventType{};
    std::uint32_t primarySchema{kAbsentIndex};
    std::uint32_t secondarySchema{kAbsentIndex};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::executableStatic};
    std::uint64_t descriptorEvidenceAddress{};
    std::uint64_t primaryEvidenceAddress{};
    std::uint64_t secondaryEvidenceAddress{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One reflected runtime schema required by a generated simulation event. */
struct RuntimeSchema final {
    std::uint32_t handle{};
    std::uint32_t decodedSize{};
    std::uint32_t definitionHash{};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::executableStatic};
    std::uint32_t definitionClass{};
    std::uint32_t codecFamilies{static_cast<std::uint32_t>(RuntimeCodecFamily::activity)};
    Range fields{};
    std::uint64_t evidenceAddress{};
    std::uint32_t flags{};
    std::uint32_t arrayElementCount{};
};

/** One exact reflected field needed by a generated runtime schema. */
struct RuntimeField final {
    std::uint32_t schemaIndex{};
    std::uint32_t ordinal{};
    std::uint32_t structOffset{};
    std::uint32_t alternateOffset{};
    std::uint32_t typeCode{};
    std::uint32_t nestedHandle{kAbsentIndex};
    std::int64_t bias{kAbsentSignedValue};
    std::uint32_t bits{kAbsentIndex};
    std::array<std::uint32_t, 4> codecParameters{};
    std::uint32_t flags{};
};

/** One universal executable reflection type selected by a six-bit union tag. */
struct RuntimeTypeDefinition final {
    StringRef name{};
    std::uint32_t codecFamilies{};
    std::uint32_t typeCode{};
    std::uint32_t decodedSize{kAbsentIndex};
    std::uint32_t fixedBits{kAbsentIndex};
    std::uint32_t minimumBits{kAbsentIndex};
    std::uint32_t maximumBits{kAbsentIndex};
    std::uint64_t writerEvidenceAddress{};
    std::uint64_t readerEvidenceAddress{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One installed SObject RSAT and its ordered component descriptor range. */
struct SobjectRsat final {
    std::uint32_t rsatTag{};
    std::uint32_t reverseDefinitionTag{kAbsentIndex};
    std::uint32_t descriptorArrayOffset{};
    std::int64_t descriptorArrayRelative{};
    std::uint32_t descriptorArrayHeaderOffset{kAbsentIndex};
    std::uint32_t descriptorArrayDataOffset{kAbsentIndex};
    std::uint32_t descriptorElementClass{kAbsentIndex};
    std::uint32_t dynamicPresenceTailCount{};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::packageField};
    Range descriptors{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One ordered component descriptor from an installed SObject RSAT. */
struct SobjectRsatDescriptor final {
    std::uint32_t rsatIndex{};
    std::uint32_t descriptorOrdinal{};
    std::uint32_t descriptorOffset{};
    std::uint32_t componentTag{};
    std::uint32_t schemaIndex{kAbsentIndex};
    std::uint32_t schemaTag{};
    std::uint32_t schemaFieldCount{};
    std::uint32_t schemaFirstFieldRuntimeGate{kAbsentIndex};
    std::uint32_t flags{};
    std::uint32_t dynamicPresenceTailOrdinal{kAbsentIndex};
    std::array<std::byte, kRsatDescriptorRawRowSize> rawRow{};
};

/** One executable-derived channel-2 entity type contract. */
struct EntityTypeDefinition final {
    StringRef name{};
    std::uint32_t entityType{};
    std::uint32_t baselineSchema{kAbsentIndex};
    std::uint32_t updateSchema{kAbsentIndex};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::executableStatic};
    std::uint64_t vtableEvidenceAddress{};
    std::uint64_t baselineEvidenceAddress{};
    std::uint64_t updateEvidenceAddress{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** Parsed executable-schema join carried by one package RSAT field row. */
struct SobjectRsatFieldBinding final {
    std::uint32_t rsatFieldIndex{};
    std::uint32_t runtimeSchemaHandle{kAbsentIndex};
    std::uint32_t parameter14{};
    std::uint32_t parameter18{};
    std::uint64_t decodedOffset{};
    std::uint32_t definitionClass{};
    std::uint32_t codecFamilies{};
    ActorSemanticProvenance provenance{ActorSemanticProvenance::packageField};
    std::uint32_t flags{};
};

/** One state-machine state name an actor class's definition declares, in authored order. */
struct ActorStateName final {
    std::uint32_t actorClassIndex{};
    std::uint32_t definitionTag{};
    std::uint32_t groupHash{};
    std::uint32_t nameHash{};
    std::uint32_t ordinal{};
    std::uint32_t flags{};
};

/** One package action table shared by all actors that select it. */
struct ActorSequenceTable final {
    std::uint32_t definitionTag{};
    std::uint32_t definitionClass{};
    std::uint32_t arrayOffset{};
    Range entries{};
    std::uint32_t flags{};
};

/** One authored action key, including unsupported kinds and exact diagnostic source text. */
struct ActorSequenceEntry final {
    StringRef id{};
    StringRef name{};
    StringRef symbol{};
    StringRef sourcePath{};
    std::uint32_t tableIndex{};
    std::uint32_t ordinal{};
    std::uint32_t sourceOffset{};
    std::uint32_t keyHash{};
    std::uint32_t kind{};
    std::uint32_t resourceTag{};
    std::uint32_t flags{};
    std::uint32_t reserved{};
};

/** One declared actor component chooses its global-first and local-fallback action tables. */
struct ActorSequenceBinding final {
    std::uint32_t actorClassIndex{};
    std::uint32_t componentOrdinal{};
    std::uint32_t configTag{};
    std::uint32_t sourceOffset{};
    std::uint32_t globalTableIndex{kAbsentIndex};
    std::uint32_t localTableIndex{kAbsentIndex};
    std::uint32_t sourceClass{};
    std::uint32_t flags{};
};

#pragma pack(pop)

static_assert(sizeof(Section) == kSectionSize);
static_assert(sizeof(Header) == kHeaderSize);
static_assert(sizeof(StringRef) == kStringRefSize);
static_assert(sizeof(Range) == kRangeSize);
static_assert(sizeof(Activity) == kActivitySize);
static_assert(sizeof(ActivityBindingTag) == kActivityBindingTagSize);
static_assert(sizeof(ActivityBindingLocator) == kActivityBindingLocatorSize);
static_assert(sizeof(Scenario) == kScenarioSize);
static_assert(sizeof(Bubble) == kBubbleSize);
static_assert(sizeof(State) == kStateSize);
static_assert(sizeof(Object) == kObjectSize);
static_assert(sizeof(Occurrence) == kOccurrenceSize);
static_assert(sizeof(Slot) == kSlotSize);
static_assert(sizeof(Text) == kTextSize);
static_assert(sizeof(Capability) == kCapabilitySize);
static_assert(sizeof(Gate) == kGateSize);
static_assert(sizeof(Refusal) == kRefusalSize);
static_assert(sizeof(ActorClass) == kActorClassSize);
static_assert(sizeof(RsatDescriptor) == kRsatDescriptorSize);
static_assert(sizeof(RsatSchema) == kRsatSchemaSize);
static_assert(sizeof(RsatField) == kRsatFieldSize);
static_assert(sizeof(Squad) == kSquadSize);
static_assert(sizeof(SquadMember) == kSquadMemberSize);
static_assert(sizeof(SquadAnchor) == kSquadAnchorSize);
static_assert(sizeof(AuthoredSceneResource) == kAuthoredSceneResourceSize);
static_assert(sizeof(AuthoredSceneSquadEdge) == kAuthoredSceneSquadEdgeSize);
static_assert(sizeof(TaskTarget) == kTaskTargetSize);
static_assert(sizeof(DialogueCueText) == kDialogueCueTextSize);
static_assert(sizeof(DirectiveElement) == kDirectiveElementSize);
static_assert(sizeof(BehaviorProgram) == kBehaviorProgramSize);
static_assert(sizeof(BehaviorInput) == kBehaviorInputSize);
static_assert(sizeof(BehaviorChannelWrite) == kBehaviorChannelWriteSize);
static_assert(sizeof(BehaviorOwner) == kBehaviorOwnerSize);
static_assert(sizeof(BehaviorActivityBinding) == kBehaviorActivityBindingSize);
static_assert(sizeof(ActorMessageSchema) == kActorMessageSchemaSize);
static_assert(sizeof(ActorCommandDefinition) == kActorCommandDefinitionSize);
static_assert(sizeof(ActorBehaviorProfile) == kActorBehaviorProfileSize);
static_assert(sizeof(SimulationEventDefinition) == kSimulationEventDefinitionSize);
static_assert(sizeof(RuntimeSchema) == kRuntimeSchemaSize);
static_assert(sizeof(RuntimeField) == kRuntimeFieldSize);
static_assert(sizeof(RuntimeTypeDefinition) == kRuntimeTypeDefinitionSize);
static_assert(sizeof(SobjectRsat) == kSobjectRsatSize);
static_assert(sizeof(SobjectRsatDescriptor) == kSobjectRsatDescriptorSize);
static_assert(sizeof(EntityTypeDefinition) == kEntityTypeDefinitionSize);
static_assert(sizeof(SobjectRsatFieldBinding) == kSobjectRsatFieldBindingSize);
static_assert(sizeof(ActorStateName) == kActorStateNameSize);
static_assert(sizeof(ActorSequenceTable) == kActorSequenceTableSize);
static_assert(sizeof(ActorSequenceEntry) == kActorSequenceEntrySize);
static_assert(sizeof(ActorSequenceBinding) == kActorSequenceBindingSize);
static_assert(std::is_trivially_copyable_v<ActorStateName>
              && std::is_standard_layout_v<ActorStateName>);
static_assert(offsetof(ActorStateName, actorClassIndex) == offset::kActorStateNameActorClassIndex);
static_assert(offsetof(ActorStateName, definitionTag) == offset::kActorStateNameDefinitionTag);
static_assert(offsetof(ActorStateName, groupHash) == offset::kActorStateNameGroupHash);
static_assert(offsetof(ActorStateName, nameHash) == offset::kActorStateNameNameHash);
static_assert(offsetof(ActorStateName, ordinal) == offset::kActorStateNameOrdinal);
static_assert(offsetof(ActorStateName, flags) == offset::kActorStateNameFlags);
static_assert(std::is_trivially_copyable_v<BehaviorProgram>
              && std::is_standard_layout_v<BehaviorProgram>);
static_assert(std::is_trivially_copyable_v<BehaviorInput>
              && std::is_standard_layout_v<BehaviorInput>);
static_assert(std::is_trivially_copyable_v<BehaviorChannelWrite>
              && std::is_standard_layout_v<BehaviorChannelWrite>);
static_assert(std::is_trivially_copyable_v<BehaviorOwner>
              && std::is_standard_layout_v<BehaviorOwner>);
static_assert(std::is_trivially_copyable_v<BehaviorActivityBinding>
              && std::is_standard_layout_v<BehaviorActivityBinding>);
static_assert(std::is_trivially_copyable_v<ActorMessageSchema>
              && std::is_standard_layout_v<ActorMessageSchema>);
static_assert(std::is_trivially_copyable_v<ActorCommandDefinition>
              && std::is_standard_layout_v<ActorCommandDefinition>);
static_assert(std::is_trivially_copyable_v<ActorBehaviorProfile>
              && std::is_standard_layout_v<ActorBehaviorProfile>);
static_assert(std::is_trivially_copyable_v<SimulationEventDefinition>
              && std::is_standard_layout_v<SimulationEventDefinition>);
static_assert(std::is_trivially_copyable_v<RuntimeSchema>
              && std::is_standard_layout_v<RuntimeSchema>);
static_assert(std::is_trivially_copyable_v<RuntimeField>
              && std::is_standard_layout_v<RuntimeField>);
static_assert(std::is_trivially_copyable_v<RuntimeTypeDefinition>
              && std::is_standard_layout_v<RuntimeTypeDefinition>);
static_assert(std::is_trivially_copyable_v<SobjectRsat> && std::is_standard_layout_v<SobjectRsat>);
static_assert(std::is_trivially_copyable_v<SobjectRsatDescriptor>
              && std::is_standard_layout_v<SobjectRsatDescriptor>);
static_assert(std::is_trivially_copyable_v<EntityTypeDefinition>
              && std::is_standard_layout_v<EntityTypeDefinition>);
static_assert(std::is_trivially_copyable_v<SobjectRsatFieldBinding>
              && std::is_standard_layout_v<SobjectRsatFieldBinding>);
static_assert(offsetof(Section, offset) == offset::kSectionOffset);
static_assert(offsetof(Section, count) == offset::kSectionCount);
static_assert(offsetof(Section, stride) == offset::kSectionStride);
static_assert(offsetof(Header, magic) == offset::kHeaderMagic);
static_assert(offsetof(Header, version) == offset::kHeaderVersion);
static_assert(offsetof(Header, headerSize) == offset::kHeaderHeaderSize);
static_assert(offsetof(Header, fileSize) == offset::kHeaderFileSize);
static_assert(offsetof(Header, payloadSha256) == offset::kHeaderPayloadSha256);
static_assert(offsetof(Header, sdkBuildSha256) == offset::kHeaderSdkBuildSha256);
static_assert(offsetof(Header, contentKeySha256) == offset::kHeaderContentKeySha256);
static_assert(offsetof(Header, logicalIrSha256) == offset::kHeaderLogicalIrSha256);
static_assert(offsetof(Header, sectionCount) == offset::kHeaderSectionCount);
static_assert(offsetof(Header, reserved) == offset::kHeaderReserved);
static_assert(offsetof(Header, sections) == offset::kHeaderSections);
static_assert(offsetof(StringRef, offset) == offset::kStringRefOffset);
static_assert(offsetof(StringRef, length) == offset::kStringRefLength);
static_assert(offsetof(Range, first) == offset::kRangeFirst);
static_assert(offsetof(Range, count) == offset::kRangeCount);
static_assert(offsetof(Activity, activityIndex) == offset::kActivityActivityIndex);
static_assert(offsetof(Activity, definitionHash) == offset::kActivityDefinitionHash);
static_assert(offsetof(Activity, id) == offset::kActivityId);
static_assert(offsetof(Activity, internalName) == offset::kActivityInternalName);
static_assert(offsetof(Activity, displayName) == offset::kActivityDisplayName);
static_assert(offsetof(Activity, scenarioIndex) == offset::kActivityScenarioIndex);
static_assert(offsetof(Activity, flags) == offset::kActivityFlags);
static_assert(offsetof(Activity, aliases) == offset::kActivityAliases);
static_assert(offsetof(Activity, capabilities) == offset::kActivityCapabilities);
static_assert(offsetof(Activity, selectedActivityRootTag)
              == offset::kActivitySelectedActivityRootTag);
static_assert(offsetof(Activity, selectedScenarioTag) == offset::kActivitySelectedScenarioTag);
static_assert(offsetof(Activity, matchmakingConfigTag) == offset::kActivityMatchmakingConfigTag);
static_assert(offsetof(Activity, joinStatus) == offset::kActivityJoinStatus);
static_assert(offsetof(Activity, bindingDisposition) == offset::kActivityBindingDisposition);
static_assert(offsetof(Activity, bindingReason) == offset::kActivityBindingReason);
static_assert(offsetof(Activity, bindingEvidenceBasis) == offset::kActivityBindingEvidenceBasis);
static_assert(offsetof(Activity, runnableStatus) == offset::kActivityRunnableStatus);
static_assert(offsetof(Activity, bindingFlags) == offset::kActivityBindingFlags);
static_assert(offsetof(Activity, activityRootCandidateTags) == offset::kActivityRootCandidateTags);
static_assert(offsetof(Activity, scenarioNameCandidateTags)
              == offset::kActivityScenarioNameCandidateTags);
static_assert(offsetof(Activity, evidenceRootTags) == offset::kActivityEvidenceRootTags);
static_assert(offsetof(Activity, bindingLocators) == offset::kActivityBindingLocators);
static_assert(offsetof(ActivityBindingTag, tag) == offset::kActivityBindingTagTag);
static_assert(offsetof(ActivityBindingLocator, tag) == offset::kActivityBindingLocatorTag);
static_assert(offsetof(ActivityBindingLocator, reserved)
              == offset::kActivityBindingLocatorReserved);
static_assert(offsetof(ActivityBindingLocator, offset) == offset::kActivityBindingLocatorOffset);
static_assert(offsetof(Scenario, tag) == offset::kScenarioTag);
static_assert(offsetof(Scenario, reserved) == offset::kScenarioReserved);
static_assert(offsetof(Scenario, id) == offset::kScenarioId);
static_assert(offsetof(Scenario, name) == offset::kScenarioName);
static_assert(offsetof(Scenario, bubbles) == offset::kScenarioBubbles);
static_assert(offsetof(Scenario, states) == offset::kScenarioStates);
static_assert(offsetof(Scenario, occurrences) == offset::kScenarioOccurrences);
static_assert(offsetof(Bubble, id) == offset::kBubbleId);
static_assert(offsetof(Bubble, name) == offset::kBubbleName);
static_assert(offsetof(Bubble, scenarioIndex) == offset::kBubbleScenarioIndex);
static_assert(offsetof(Bubble, bubbleOrdinal) == offset::kBubbleBubbleOrdinal);
static_assert(offsetof(Bubble, nameHash) == offset::kBubbleNameHash);
static_assert(offsetof(Bubble, reserved) == offset::kBubbleReserved);
static_assert(offsetof(Bubble, states) == offset::kBubbleStates);
static_assert(offsetof(State, id) == offset::kStateId);
static_assert(offsetof(State, entryId) == offset::kStateEntryId);
static_assert(offsetof(State, registryId) == offset::kStateRegistryId);
static_assert(offsetof(State, scenarioIndex) == offset::kStateScenarioIndex);
static_assert(offsetof(State, bubbleIndex) == offset::kStateBubbleIndex);
static_assert(offsetof(State, stateOrdinal) == offset::kStateStateOrdinal);
static_assert(offsetof(State, entryIndex) == offset::kStateEntryIndex);
static_assert(offsetof(State, sliceSetIndex) == offset::kStateSliceSetIndex);
static_assert(offsetof(State, mapBubbleIndex) == offset::kStateMapBubbleIndex);
static_assert(offsetof(State, stateHash) == offset::kStateStateHash);
static_assert(offsetof(State, publicValue) == offset::kStatePublicValue);
static_assert(offsetof(State, flags) == offset::kStateFlags);
static_assert(offsetof(State, registryTag) == offset::kStateRegistryTag);
static_assert(offsetof(Object, id) == offset::kObjectId);
static_assert(offsetof(Object, objectTag) == offset::kObjectObjectTag);
static_assert(offsetof(Object, objectKey) == offset::kObjectObjectKey);
static_assert(offsetof(Object, slots) == offset::kObjectSlots);
static_assert(offsetof(Object, configCount) == offset::kObjectConfigCount);
static_assert(offsetof(Object, descriptorCount) == offset::kObjectDescriptorCount);
static_assert(offsetof(Object, placedSubblockCount) == offset::kObjectPlacedSubblockCount);
static_assert(offsetof(Object, placedLeafCount) == offset::kObjectPlacedLeafCount);
static_assert(offsetof(Object, placedHopCount) == offset::kObjectPlacedHopCount);
static_assert(offsetof(Object, bareTargetCount) == offset::kObjectBareTargetCount);
static_assert(offsetof(Occurrence, id) == offset::kOccurrenceId);
static_assert(offsetof(Occurrence, contextRegistryKey) == offset::kOccurrenceContextRegistryKey);
static_assert(offsetof(Occurrence, registryId) == offset::kOccurrenceRegistryId);
static_assert(offsetof(Occurrence, entryId) == offset::kOccurrenceEntryId);
static_assert(offsetof(Occurrence, scenarioIndex) == offset::kOccurrenceScenarioIndex);
static_assert(offsetof(Occurrence, bubbleIndex) == offset::kOccurrenceBubbleIndex);
static_assert(offsetof(Occurrence, stateIndex) == offset::kOccurrenceStateIndex);
static_assert(offsetof(Occurrence, objectIndex) == offset::kOccurrenceObjectIndex);
static_assert(offsetof(Occurrence, registryField) == offset::kOccurrenceRegistryField);
static_assert(offsetof(Occurrence, objectOrdinal) == offset::kOccurrenceObjectOrdinal);
static_assert(offsetof(Slot, id) == offset::kSlotId);
static_assert(offsetof(Slot, name) == offset::kSlotName);
static_assert(offsetof(Slot, senseSchemaId) == offset::kSlotSenseSchemaId);
static_assert(offsetof(Slot, authSchemaId) == offset::kSlotAuthSchemaId);
static_assert(offsetof(Slot, objectIndex) == offset::kSlotObjectIndex);
static_assert(offsetof(Slot, slotIndex) == offset::kSlotSlotIndex);
static_assert(offsetof(Slot, slotType) == offset::kSlotSlotType);
static_assert(offsetof(Slot, componentClass) == offset::kSlotComponentClass);
static_assert(offsetof(Slot, senseSchema) == offset::kSlotSenseSchema);
static_assert(offsetof(Slot, authSchema) == offset::kSlotAuthSchema);
static_assert(offsetof(Slot, flags) == offset::kSlotFlags);
static_assert(offsetof(Slot, reserved) == offset::kSlotReserved);
static_assert(offsetof(Slot, aliases) == offset::kSlotAliases);
static_assert(offsetof(Slot, capabilities) == offset::kSlotCapabilities);
static_assert(offsetof(Text, value) == offset::kTextValue);
static_assert(offsetof(Text, kind) == offset::kTextKind);
static_assert(offsetof(Text, reserved) == offset::kTextReserved);
static_assert(offsetof(Capability, id) == offset::kCapabilityId);
static_assert(offsetof(Capability, operation) == offset::kCapabilityOperation);
static_assert(offsetof(Capability, valueSchemaId) == offset::kCapabilityValueSchemaId);
static_assert(offsetof(Capability, subjectKind) == offset::kCapabilitySubjectKind);
static_assert(offsetof(Capability, subjectIndex) == offset::kCapabilitySubjectIndex);
static_assert(offsetof(Capability, exposureFlags) == offset::kCapabilityExposureFlags);
static_assert(offsetof(Capability, candidateExposureFlags)
              == offset::kCapabilityCandidateExposureFlags);
static_assert(offsetof(Capability, gates) == offset::kCapabilityGates);
static_assert(offsetof(Capability, refusals) == offset::kCapabilityRefusals);
static_assert(offsetof(Gate, gate) == offset::kGateGate);
static_assert(offsetof(Gate, status) == offset::kGateStatus);
static_assert(offsetof(Gate, reasonCode) == offset::kGateReasonCode);
static_assert(offsetof(Gate, required) == offset::kGateRequired);
static_assert(offsetof(Gate, observed) == offset::kGateObserved);
static_assert(offsetof(Gate, wouldConfirm) == offset::kGateWouldConfirm);
static_assert(offsetof(Refusal, id) == offset::kRefusalId);
static_assert(offsetof(Refusal, exposure) == offset::kRefusalExposure);
static_assert(offsetof(Refusal, status) == offset::kRefusalStatus);
static_assert(offsetof(Refusal, reasonCodes) == offset::kRefusalReasonCodes);
static_assert(offsetof(Refusal, capabilityIndex) == offset::kRefusalCapabilityIndex);
static_assert(offsetof(Refusal, reserved) == offset::kRefusalReserved);
static_assert(offsetof(ActorClass, id) == offset::kActorClassId);
static_assert(offsetof(ActorClass, definitionTag) == offset::kActorClassDefinitionTag);
static_assert(offsetof(ActorClass, nameHash) == offset::kActorClassNameHash);
static_assert(offsetof(ActorClass, rsatTag) == offset::kActorClassRsatTag);
static_assert(offsetof(ActorClass, rsatReverseDefinitionTag)
              == offset::kActorClassRsatReverseDefinitionTag);
static_assert(offsetof(ActorClass, objectType) == offset::kActorClassObjectType);
static_assert(offsetof(ActorClass, descriptorArrayOffset)
              == offset::kActorClassDescriptorArrayOffset);
static_assert(offsetof(ActorClass, descriptorArrayRelative)
              == offset::kActorClassDescriptorArrayRelative);
static_assert(offsetof(ActorClass, descriptorArrayHeaderOffset)
              == offset::kActorClassDescriptorArrayHeaderOffset);
static_assert(offsetof(ActorClass, descriptorArrayDataOffset)
              == offset::kActorClassDescriptorArrayDataOffset);
static_assert(offsetof(ActorClass, descriptorElementClass)
              == offset::kActorClassDescriptorElementClass);
static_assert(offsetof(ActorClass, descriptors) == offset::kActorClassDescriptors);
static_assert(offsetof(ActorClass, dynamicPresenceTailCount)
              == offset::kActorClassDynamicPresenceTailCount);
static_assert(offsetof(ActorClass, authoredSpawnProfile) == offset::kActorClassReserved);
static_assert(offsetof(RsatDescriptor, id) == offset::kRsatDescriptorId);
static_assert(offsetof(RsatDescriptor, actorClassIndex) == offset::kRsatDescriptorActorClassIndex);
static_assert(offsetof(RsatDescriptor, rsatTag) == offset::kRsatDescriptorRsatTag);
static_assert(offsetof(RsatDescriptor, descriptorOrdinal) == offset::kRsatDescriptorOrdinal);
static_assert(offsetof(RsatDescriptor, descriptorOffset) == offset::kRsatDescriptorOffset);
static_assert(offsetof(RsatDescriptor, descriptorElementClass)
              == offset::kRsatDescriptorElementClass);
static_assert(offsetof(RsatDescriptor, componentTag) == offset::kRsatDescriptorComponentTag);
static_assert(offsetof(RsatDescriptor, schemaIndex) == offset::kRsatDescriptorSchemaIndex);
static_assert(offsetof(RsatDescriptor, schemaTag) == offset::kRsatDescriptorSchemaTag);
static_assert(offsetof(RsatDescriptor, schemaFieldCount)
              == offset::kRsatDescriptorSchemaFieldCount);
static_assert(offsetof(RsatDescriptor, schemaFirstFieldRuntimeGate)
              == offset::kRsatDescriptorSchemaFirstFieldRuntimeGate);
static_assert(offsetof(RsatDescriptor, schemaFirstFieldRawU32At10)
              == offset::kRsatDescriptorSchemaFirstFieldRawU32At10);
static_assert(offsetof(RsatDescriptor, flags) == offset::kRsatDescriptorFlags);
static_assert(offsetof(RsatDescriptor, dynamicPresenceTailOrdinal)
              == offset::kRsatDescriptorDynamicPresenceTailOrdinal);
static_assert(offsetof(RsatDescriptor, rawRow) == offset::kRsatDescriptorRawRow);
static_assert(offsetof(RsatSchema, id) == offset::kRsatSchemaId);
static_assert(offsetof(RsatSchema, schemaTag) == offset::kRsatSchemaTag);
static_assert(offsetof(RsatSchema, schemaClass) == offset::kRsatSchemaClass);
static_assert(offsetof(RsatSchema, fieldCount) == offset::kRsatSchemaFieldCount);
static_assert(offsetof(RsatSchema, fieldArrayOffset) == offset::kRsatSchemaFieldArrayOffset);
static_assert(offsetof(RsatSchema, fieldArrayRelative) == offset::kRsatSchemaFieldArrayRelative);
static_assert(offsetof(RsatSchema, fieldArrayHeaderOffset)
              == offset::kRsatSchemaFieldArrayHeaderOffset);
static_assert(offsetof(RsatSchema, fieldArrayDataOffset)
              == offset::kRsatSchemaFieldArrayDataOffset);
static_assert(offsetof(RsatSchema, fieldElementClass) == offset::kRsatSchemaFieldElementClass);
static_assert(offsetof(RsatSchema, firstFieldRuntimeGate)
              == offset::kRsatSchemaFirstFieldRuntimeGate);
static_assert(offsetof(RsatSchema, firstFieldRawU32At10)
              == offset::kRsatSchemaFirstFieldRawU32At10);
static_assert(offsetof(RsatSchema, flags) == offset::kRsatSchemaFlags);
static_assert(offsetof(RsatSchema, fields) == offset::kRsatSchemaFields);
static_assert(offsetof(RsatField, rawRow) == offset::kRsatFieldRawRow);
static_assert(offsetof(Squad, id) == offset::kSquadId);
static_assert(offsetof(Squad, scenarioIndex) == offset::kSquadScenarioIndex);
static_assert(offsetof(Squad, objectIndex) == offset::kSquadObjectIndex);
static_assert(offsetof(Squad, slotIndex) == offset::kSquadSlotIndex);
static_assert(offsetof(Squad, spawnerConfigTag) == offset::kSquadSpawnerConfigTag);
static_assert(offsetof(Squad, spawnRuleConfigTag) == offset::kSquadSpawnRuleConfigTag);
static_assert(offsetof(Squad, flags) == offset::kSquadFlags);
static_assert(offsetof(Squad, occurrenceIndex) == offset::kSquadOccurrenceIndex);
static_assert(offsetof(Squad, members) == offset::kSquadMembers);
static_assert(offsetof(Squad, anchors) == offset::kSquadAnchors);
static_assert(offsetof(SquadMember, id) == offset::kSquadMemberId);
static_assert(offsetof(SquadMember, squadIndex) == offset::kSquadMemberSquadIndex);
static_assert(offsetof(SquadMember, memberOrdinal) == offset::kSquadMemberMemberOrdinal);
static_assert(offsetof(SquadMember, memberKey) == offset::kSquadMemberMemberKey);
static_assert(offsetof(SquadMember, actorClassIndex) == offset::kSquadMemberActorClassIndex);
static_assert(offsetof(SquadMember, flags) == offset::kSquadMemberFlags);
static_assert(offsetof(SquadMember, candidateCounts) == offset::kSquadMemberCandidateCounts);
static_assert(offsetof(SquadMember, defaultCount) == offset::kSquadMemberDefaultCount);
static_assert(offsetof(SquadAnchor, id) == offset::kSquadAnchorId);
static_assert(offsetof(SquadAnchor, squadIndex) == offset::kSquadAnchorSquadIndex);
static_assert(offsetof(SquadAnchor, pointOrdinal) == offset::kSquadAnchorPointOrdinal);
static_assert(offsetof(SquadAnchor, objectListTag) == offset::kSquadAnchorObjectListTag);
static_assert(offsetof(SquadAnchor, placementOrdinal) == offset::kSquadAnchorPlacementOrdinal);
static_assert(offsetof(SquadAnchor, flags) == offset::kSquadAnchorFlags);
static_assert(offsetof(SquadAnchor, placedEntryIdentity)
              == offset::kSquadAnchorPlacedEntryIdentity);
static_assert(offsetof(SquadAnchor, positionBits) == offset::kSquadAnchorPositionBits);
static_assert(offsetof(AuthoredSceneResource, id) == offset::kAuthoredSceneResourceId);
static_assert(offsetof(AuthoredSceneResource, slotIndex)
              == offset::kAuthoredSceneResourceSlotIndex);
static_assert(offsetof(AuthoredSceneResource, configTag)
              == offset::kAuthoredSceneResourceConfigTag);
static_assert(offsetof(AuthoredSceneResource, descriptorOffset)
              == offset::kAuthoredSceneResourceDescriptorOffset);
static_assert(offsetof(AuthoredSceneResource, resourceFieldOffset)
              == offset::kAuthoredSceneResourceFieldOffset);
static_assert(offsetof(AuthoredSceneResource, resourceTag) == offset::kAuthoredSceneResourceTag);
static_assert(offsetof(AuthoredSceneResource, resourceClass)
              == offset::kAuthoredSceneResourceClass);
static_assert(offsetof(AuthoredSceneResource, flags) == offset::kAuthoredSceneResourceFlags);
static_assert(offsetof(AuthoredSceneResource, reserved) == offset::kAuthoredSceneResourceReserved);
static_assert(offsetof(AuthoredSceneSquadEdge, id) == offset::kAuthoredSceneSquadEdgeId);
static_assert(offsetof(AuthoredSceneSquadEdge, sceneSlotIndex)
              == offset::kAuthoredSceneSquadEdgeSceneSlotIndex);
static_assert(offsetof(AuthoredSceneSquadEdge, squadSlotIndex)
              == offset::kAuthoredSceneSquadEdgeSquadSlotIndex);
static_assert(offsetof(AuthoredSceneSquadEdge, configTag)
              == offset::kAuthoredSceneSquadEdgeConfigTag);
static_assert(offsetof(AuthoredSceneSquadEdge, descriptorOffset)
              == offset::kAuthoredSceneSquadEdgeDescriptorOffset);
static_assert(offsetof(AuthoredSceneSquadEdge, referenceFieldOffset)
              == offset::kAuthoredSceneSquadEdgeReferenceFieldOffset);
static_assert(offsetof(AuthoredSceneSquadEdge, targetObjectKey)
              == offset::kAuthoredSceneSquadEdgeTargetObjectKey);
static_assert(offsetof(AuthoredSceneSquadEdge, flags) == offset::kAuthoredSceneSquadEdgeFlags);
static_assert(offsetof(AuthoredSceneSquadEdge, reserved)
              == offset::kAuthoredSceneSquadEdgeReserved);
static_assert(offsetof(TaskTarget, id) == offset::kTaskTargetId);
static_assert(offsetof(TaskTarget, taskSlotIndex) == offset::kTaskTargetTaskSlotIndex);
static_assert(offsetof(TaskTarget, objectiveSlotIndex) == offset::kTaskTargetObjectiveSlotIndex);
static_assert(offsetof(TaskTarget, configTag) == offset::kTaskTargetConfigTag);
static_assert(offsetof(TaskTarget, descriptorOffset) == offset::kTaskTargetDescriptorOffset);
static_assert(offsetof(TaskTarget, referenceFieldOffset)
              == offset::kTaskTargetReferenceFieldOffset);
static_assert(offsetof(TaskTarget, targetObjectKey) == offset::kTaskTargetTargetObjectKey);
static_assert(offsetof(TaskTarget, bitIndex) == offset::kTaskTargetBitIndex);
static_assert(offsetof(TaskTarget, flags) == offset::kTaskTargetFlags);
static_assert(offsetof(TaskTarget, reserved) == offset::kTaskTargetReserved);

} // namespace sunrise::state::activity_sdk::format
