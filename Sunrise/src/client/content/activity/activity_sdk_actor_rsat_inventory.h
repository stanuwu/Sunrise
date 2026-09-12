#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity_sdk/format.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {

/** Tag class of an actor definition row. */
inline constexpr std::uint32_t kActorClassDefinitionClass = 0x80809C0FU;
/** Tag class of the reciprocal RSAT row named by an actor definition. */
inline constexpr std::uint32_t kActorRsatClass = 0x80809BB6U;
/** Actor definitions must contain every field used by this projection. */
inline constexpr std::size_t kActorDefinitionMinimumSize = 0x97U;
/** Actor definitions carry their name hash at this byte offset. */
inline constexpr std::size_t kActorNameHashOffset = 0x08U;
/** Actor definitions carry the four values forwarded by type-1 Auth field 5 at +56..+59. */
inline constexpr std::size_t kActorAuthoredSpawnProfileOffset = 0x38U;
/** Actor definitions name their RSAT row at this byte offset. */
inline constexpr std::size_t kActorRsatTagOffset = 0x88U;
/** Actor definitions carry their one-byte object type at this byte offset. */
inline constexpr std::size_t kActorObjectTypeOffset = 0x96U;
/** Actor definitions name their behavior configuration at this byte offset. */
inline constexpr std::size_t kActorBehaviorConfigOffset = 0x300U;
/** RSAT rows name their owning actor definition at this byte offset. */
inline constexpr std::size_t kActorRsatReverseDefinitionOffset = 0x08U;
/** Exact descriptor and schema-field strides in package data. */
inline constexpr std::size_t kDescriptorStride = 0x20U;
inline constexpr std::size_t kSchemaFieldStride = 0x28U;
/** Deferred SDK strings must fit this fixed storage including their terminator. */
inline constexpr std::size_t kTextCapacity = 48U;

/** One owned string retained until the final pack string table is linked. */
struct Text final {
    std::array<char, kTextCapacity> value{};
    std::uint16_t length{};
};

/** One exact format-v12 actor-class row before its id is linked. */
struct ActorClass final {
    Text id{};
    std::uint32_t definitionTag{};
    std::uint32_t nameHash{};
    std::uint32_t rsatTag{};
    std::uint32_t rsatReverseDefinitionTag{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t objectType{};
    std::uint32_t descriptorArrayOffset{state::activity_sdk::format::kAbsentIndex};
    std::int64_t descriptorArrayRelative{state::activity_sdk::format::kAbsentRelativeOffset};
    std::uint32_t descriptorArrayHeaderOffset{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t descriptorArrayDataOffset{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t descriptorElementClass{state::activity_sdk::format::kAbsentIndex};
    state::activity_sdk::format::Range descriptors{};
    std::uint32_t dynamicPresenceTailCount{};
    std::array<std::int8_t, 4> authoredSpawnProfile{};
};

/** One exact format-v12 RSAT descriptor row before its id is linked. */
struct RsatDescriptor final {
    Text id{};
    std::uint32_t actorClassIndex{};
    std::uint32_t rsatTag{};
    std::uint32_t descriptorOrdinal{};
    std::uint32_t descriptorOffset{};
    std::uint32_t descriptorElementClass{};
    std::uint32_t componentTag{};
    std::uint32_t schemaIndex{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t schemaTag{};
    std::uint32_t schemaFieldCount{};
    std::uint32_t schemaFirstFieldRuntimeGate{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t schemaFirstFieldRawU32At10{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t flags{};
    std::uint32_t dynamicPresenceTailOrdinal{state::activity_sdk::format::kAbsentIndex};
    std::array<std::byte, state::activity_sdk::format::kRsatDescriptorRawRowSize> rawRow{};
};

/** One exact format-v12 RSAT schema row before its id is linked. */
struct RsatSchema final {
    Text id{};
    std::uint32_t schemaTag{};
    std::uint32_t schemaClass{};
    std::uint32_t fieldCount{};
    std::uint32_t fieldArrayOffset{};
    std::int64_t fieldArrayRelative{};
    std::uint32_t fieldArrayHeaderOffset{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t fieldArrayDataOffset{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t fieldElementClass{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t firstFieldRuntimeGate{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t firstFieldRawU32At10{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t flags{};
    state::activity_sdk::format::Range fields{};
};

/** One exact format-v12 RSAT field row. */
struct RsatField final {
    std::array<std::byte, state::activity_sdk::format::kRsatSchemaFieldRawRowSize> rawRow{};
};

/** One versioned executable-derived actor command message. */
struct ActorMessageSchema final {
    Text name{};
    std::uint32_t definitionHandle{};
    std::uint32_t durableKey{};
    std::uint32_t ownerClass{};
    std::uint32_t handlerSlot{};
    std::uint32_t bodyType{};
    state::activity_sdk::format::ActorSemanticProvenance provenance{
        state::activity_sdk::format::ActorSemanticProvenance::executableStatic};
    state::activity_sdk::format::Range commands{};
    std::uint32_t flags{};
};

/** One versioned executable-derived actor command definition. */
struct ActorCommandDefinition final {
    Text name{};
    Text factionNoneName{};
    Text factionRemovedName{};
    Text factionHostileToAllName{};
    std::uint32_t selector{};
    std::uint32_t payloadHandle{};
    state::activity_sdk::format::ActorCommandEffect effect{
        state::activity_sdk::format::ActorCommandEffect::opaque};
    state::activity_sdk::format::ActorSemanticProvenance provenance{
        state::activity_sdk::format::ActorSemanticProvenance::executableStatic};
    std::int32_t factionNone{};
    std::int32_t factionRemoved{};
    std::int32_t factionHostileToAll{};
    std::uint32_t flags{};
};

/** One package behavior configuration and the engine faction default for an actor. */
struct ActorBehaviorProfile final {
    std::uint32_t actorClassIndex{};
    std::uint32_t behaviorConfigTag{};
    std::uint32_t behaviorConfigClass{};
    std::uint32_t behaviorConfigOffset{};
    std::int32_t defaultFaction{};
    state::activity_sdk::format::ActorSemanticProvenance behaviorProvenance{
        state::activity_sdk::format::ActorSemanticProvenance::packageField};
    state::activity_sdk::format::ActorSemanticProvenance factionProvenance{
        state::activity_sdk::format::ActorSemanticProvenance::engineZeroDefault};
    std::uint32_t flags{};
};

/** One lane-0 simulation event. A schema field holds the absent index when none was found. */
struct SimulationEventDefinition final {
    Text name{};
    std::uint32_t eventType{};
    std::uint32_t primarySchema{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t secondarySchema{state::activity_sdk::format::kAbsentIndex};
    state::activity_sdk::format::ActorSemanticProvenance provenance{
        state::activity_sdk::format::ActorSemanticProvenance::executableStatic};
    std::uint64_t descriptorEvidenceAddress{};
    std::uint64_t primaryEvidenceAddress{};
    std::uint64_t secondaryEvidenceAddress{};
    std::uint32_t flags{};
};

using RuntimeSchema = state::activity_sdk::format::RuntimeSchema;
using RuntimeField = state::activity_sdk::format::RuntimeField;

/** One executable reflection type before its stable name is linked. */
struct RuntimeTypeDefinition final {
    Text name{};
    std::uint32_t codecFamilies{};
    std::uint32_t typeCode{};
    std::uint32_t decodedSize{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t fixedBits{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t minimumBits{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t maximumBits{state::activity_sdk::format::kAbsentIndex};
    std::uint64_t writerEvidenceAddress{};
    std::uint64_t readerEvidenceAddress{};
    std::uint32_t flags{};
};
using SobjectRsat = state::activity_sdk::format::SobjectRsat;
using SobjectRsatDescriptor = state::activity_sdk::format::SobjectRsatDescriptor;
using SobjectRsatFieldBinding = state::activity_sdk::format::SobjectRsatFieldBinding;
using ActorStateName = state::activity_sdk::format::ActorStateName;

/** One extracted channel-2 entity type contract before its name is linked. */
struct EntityTypeDefinition final {
    Text name{};
    std::uint32_t entityType{};
    std::uint32_t baselineSchema{state::activity_sdk::format::kAbsentIndex};
    std::uint32_t updateSchema{state::activity_sdk::format::kAbsentIndex};
    state::activity_sdk::format::ActorSemanticProvenance provenance{
        state::activity_sdk::format::ActorSemanticProvenance::executableStatic};
    std::uint64_t vtableEvidenceAddress{};
    std::uint64_t baselineEvidenceAddress{};
    std::uint64_t updateEvidenceAddress{};
    std::uint32_t flags{};
};

/** One complete installed definition selected by the sequence-table class scan. */
struct SequenceTableSource final {
    std::uint32_t definitionTag{};
    std::uint32_t definitionClass{};
};

/** Strings remain owned until the immutable pack string table is linked. */
struct SequenceEntry final {
    state::activity_sdk::format::ActorSequenceEntry row{};
    std::string id{};
    std::string name{};
    std::string symbol{};
    std::string sourcePath{};
};

/** Complete native actor and RSAT inventory with unresolved string references. */
struct Snapshot final {
    std::vector<ActorClass> actorClasses{};
    std::vector<RsatDescriptor> descriptors{};
    std::vector<RsatSchema> schemas{};
    std::vector<RsatField> fields{};
    std::vector<ActorMessageSchema> messageSchemas{};
    std::vector<ActorCommandDefinition> commandDefinitions{};
    std::vector<ActorBehaviorProfile> behaviorProfiles{};
    std::vector<SimulationEventDefinition> simulationEvents{};
    std::vector<RuntimeSchema> runtimeSchemas{};
    std::vector<RuntimeField> runtimeFields{};
    std::vector<RuntimeTypeDefinition> runtimeTypes{};
    std::vector<SobjectRsat> sobjectRsats{};
    std::vector<SobjectRsatDescriptor> sobjectRsatDescriptors{};
    std::vector<EntityTypeDefinition> entityTypes{};
    std::vector<SobjectRsatFieldBinding> sobjectRsatFieldBindings{};
    /** Sorted by actor class then authored ordinal; the group is always `state_machine`. */
    std::vector<ActorStateName> actorStateNames{};
    std::vector<state::activity_sdk::format::ActorSequenceTable> sequenceTables{};
    std::vector<SequenceEntry> sequenceEntries{};
    std::vector<state::activity_sdk::format::ActorSequenceBinding> sequenceBindings{};
    bool complete{};
};

/** Optional cancellation probe used by the installed package walk. */
using CancelProbe = bool (*)(void* context) noexcept;

/** Testable package read boundary. The callback must enforce the requested class. */
using ReadTag = bool (*)(void* context,
                         std::uint32_t tag,
                         std::uint32_t expectedClass,
                         std::vector<std::byte>& output) noexcept;

/** Validates every id, scalar, raw row, owner, range, order, join, and flag. */
[[nodiscard]] bool validate(const Snapshot& snapshot) noexcept;

/** Builds an inventory from an already complete, sorted actor-definition tag set. */
[[nodiscard]] bool build_from_tags(std::span<const std::uint32_t> actorTags,
                                   ReadTag readTag,
                                   void* readContext,
                                   CancelProbe cancel,
                                   void* cancelContext,
                                   Snapshot& output) noexcept;

/** Builds actor joins plus a complete caller-supplied installed RSAT tag set. */
[[nodiscard]] bool
build_from_tags_and_rsats(std::span<const std::uint32_t> actorTags,
                          std::span<const std::uint32_t> rsatTags,
                          ReadTag readTag,
                          void* readContext,
                          CancelProbe cancel,
                          void* cancelContext,
                          Snapshot& output,
                          std::span<const SequenceTableSource> sequenceTables = {}) noexcept;

/** Builds a structurally complete installed inventory for an upstream exact actor tag set. */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         std::span<const std::uint32_t> actorTags,
                         CancelProbe cancel,
                         void* cancelContext,
                         Snapshot& output) noexcept;

/** Builds actor joins against one complete sorted installed RSAT tag set. */
[[nodiscard]] bool build_with_rsats(const middleware::content::packages::reader::Source& source,
                                    std::span<const std::uint32_t> actorTags,
                                    std::span<const std::uint32_t> rsatTags,
                                    CancelProbe cancel,
                                    void* cancelContext,
                                    Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
