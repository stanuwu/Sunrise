#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../state/activity_sdk/format.h"
#include "activity_sdk_external_placements.h"
#include "activity_sdk_spawn_reference.h"
#include "activity_sdk_topology_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {

namespace format = state::activity_sdk::format;

/** A caller-owned reader returns one complete package entry and its physical class. */
using TagReader = bool (*)(void* context,
                           std::uint32_t tag,
                           std::vector<std::byte>& bytes,
                           std::uint32_t& classId) noexcept;

/** The actor inventory may close one exact definition tag to its final row index. */
using ActorResolver = bool (*)(void* context,
                               std::uint32_t definitionTag,
                               std::uint32_t& actorClassIndex,
                               std::array<std::int8_t, 4>& authoredSpawnProfile) noexcept;

enum class CandidateState : std::uint8_t {
    nullPlacement,
    exactPlacement,
};

/**
 * One authored candidate, holding the whole stable package projection.
 * The runtime squad view reads only `actorDefinitionTag` and `state`; generation needs the rest to
 * reproduce the spawner graph. Float lanes stay as bits so normalization cannot move the identity.
 */
struct CandidateFact final {
    std::uint32_t actorDefinitionTag{format::kAbsentIndex};
    CandidateState state{CandidateState::nullPlacement};
    std::uint64_t candidateDescriptorOffset{};
    std::int64_t placementRelative{};
    std::uint64_t placementOffset{};
    std::array<std::byte, 16> candidateTail{};
    std::uint32_t placedEntryClass{format::kAbsentIndex};
    std::array<std::uint32_t, 4> quaternionBits{};
    std::array<std::uint32_t, 3> positionBits{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::uint64_t placedEntryIdentity{};

    [[nodiscard]] bool operator==(const CandidateFact&) const noexcept = default;
};

/** One authored member owns six ordered candidate lanes. */
struct MemberFact final {
    std::uint32_t memberKey{};
    std::array<std::vector<CandidateFact>, format::kSquadCandidateCountLaneCount> candidates{};
    std::uint32_t reservedU32{};
};

/** One ordered authored point from a type-66 config or a spawner's own point set. */
struct RulePointFact final {
    std::uint64_t placedEntryIdentity{};
    std::uint64_t rowOffset{};
    std::array<std::byte, 64> rawTail{};

    [[nodiscard]] bool operator==(const RulePointFact&) const noexcept = default;
};

/** One exact type-1 config before its scenario occurrence is joined. */
struct SpawnerFact final {
    std::uint32_t configTag{};
    std::uint64_t rawReference98{};
    std::uint64_t rawReferenceA0{};
    std::vector<MemberFact> members{};
    bool complete{};
    std::uint64_t primaryComponentOffset{};
    std::uint64_t secondaryComponentOffset{};
    std::uint32_t primaryComponentClass{};
    std::uint32_t secondaryComponentClass{};
    /** Set when the spawner carries its own point set instead of a type-66 reference. */
    bool hasInlinePointSet{};
    /** Successful canonical parsing distinguishes absent from unread inline data. */
    bool inlinePointSetInspected{};
    std::uint64_t inlinePointSetOffset{};
    std::uint64_t inlinePlacementComponentOffset{};
    std::uint32_t inlineInitialPointIndex{};
    std::vector<RulePointFact> inlinePoints{};
    CandidateFact inlinePlacement{};
};

/**
 * One exact type-66 config before point identities are contextualized. An inline rule is the
 * spawner's own point set under the spawner's config tag.
 */
struct RuleFact final {
    std::uint32_t configTag{};
    std::vector<RulePointFact> points{};
    bool complete{};
    std::uint64_t primaryComponentOffset{};
    std::uint64_t secondaryComponentOffset{};
    std::uint32_t primaryComponentClass{};
    std::uint32_t secondaryComponentClass{};
    bool inlineForm{};
};

/** One exact package descriptor attached to one canonical object slot. */
struct DescriptorFact final {
    std::string id{};
    std::uint32_t configTag{};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::uint32_t slotIndex{format::kAbsentIndex};
    std::uint32_t descriptorOffset{};
    std::uint32_t componentClass{};
    std::uint32_t senseSchema{};
    std::uint32_t authSchema{};
    bool complete{};
};

/** One path-specific placed config after the authored bubble rule matched an occurrence. */
struct ConfigOccurrenceFact final {
    std::string id{};
    std::uint32_t configTag{};
    std::uint32_t occurrenceIndex{format::kAbsentIndex};
    bool complete{};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::uint32_t pathOrdinal{};
    std::int32_t declaredBubbleIndex{};
};

/** One path-specific object-list row after the authored bubble rule matched an occurrence. */
struct PlacementOccurrenceFact final {
    std::string id{};
    std::uint32_t occurrenceIndex{format::kAbsentIndex};
    /**
     * Set when the placement came from a container list or a type-4 descriptor.
     * Such a row belongs to no activity object occurrence, so it carries its scenario directly
     * and the occurrence, object and bubble checks do not apply to it.
     */
    bool external{};
    std::uint32_t scenarioIndex{format::kAbsentIndex};
    std::uint32_t objectListTag{};
    std::uint32_t placementOrdinal{};
    std::uint64_t placedEntryIdentity{};
    std::array<std::uint32_t, 3> positionBits{};
    bool complete{};
    std::uint32_t objectIndex{format::kAbsentIndex};
    std::uint32_t pathOrdinal{};
    std::int32_t declaredBubbleIndex{};
    std::uint32_t actorDefinitionTag{format::kAbsentIndex};
    std::uint64_t sourceOffset{};
    std::array<std::uint32_t, 4> quaternionBits{};
    std::uint32_t uniformScaleBits{};
    std::uint32_t nameHash{};
    std::uint32_t placementFlagsRaw{};
    std::int64_t auxiliaryRelative{};
};

/** A separate exact schema join supplies the final source-slot tuple. */
struct SlotSchemaFact final {
    std::uint32_t slotIndex{format::kAbsentIndex};
    std::uint32_t componentClass{format::kAbsentIndex};
    std::uint32_t senseSchema{format::kAbsentIndex};
    std::uint32_t authSchema{format::kAbsentIndex};
    bool exact{};
};

/** All package and linker facts consumed by the deterministic squad projection. */
struct Facts final {
    std::vector<SpawnerFact> spawners{};
    std::vector<RuleFact> rules{};
    std::vector<DescriptorFact> descriptors{};
    std::vector<ConfigOccurrenceFact> configOccurrences{};
    std::vector<PlacementOccurrenceFact> placementOccurrences{};
    std::vector<SlotSchemaFact> slotSchemas{};
    /** Exact reached definition tags feed the separate actor/RSAT inventory. */
    std::vector<std::uint32_t> actorDefinitionTags{};
    std::uint64_t unresolvedReads{};
    bool complete{};
};

enum class ActorLink : std::uint8_t {
    absent,
    pendingDefinition,
    exactReciprocal,
    inconsistentDefinitions,
};

/** One scenario-local type-1 source and its contiguous child ranges. */
struct Squad final {
    std::string id{};
    std::uint32_t scenarioIndex{};
    std::uint32_t objectIndex{};
    std::uint32_t slotIndex{};
    std::uint32_t spawnerConfigTag{};
    std::uint32_t spawnRuleConfigTag{};
    std::uint32_t flags{};
    std::uint32_t occurrenceIndex{};
    format::Range members{};
    format::Range anchors{};
};

/** One authored member with an explicit pending actor-definition link. */
struct SquadMember final {
    std::string id{};
    std::string actorDefinitionId{};
    std::uint32_t squadIndex{};
    std::uint32_t memberOrdinal{};
    std::uint32_t memberKey{};
    std::uint32_t actorClassIndex{format::kAbsentIndex};
    std::uint32_t actorDefinitionTag{format::kAbsentIndex};
    std::uint32_t flags{};
    std::array<std::uint16_t, format::kSquadCandidateCountLaneCount> candidateCounts{};
    std::int32_t defaultCount{-1};
    std::array<std::int8_t, 4> authoredSpawnProfile{};
    ActorLink actorLink{ActorLink::absent};
};

/** One exact authored point joined to one occurrence-local placement row. */
struct SquadAnchor final {
    std::string id{};
    std::uint32_t squadIndex{};
    std::uint32_t pointOrdinal{};
    std::uint32_t objectListTag{};
    std::uint32_t placementOrdinal{};
    std::uint32_t flags{};
    std::uint64_t placedEntryIdentity{};
    std::array<std::uint32_t, 3> positionBits{};
};

/** Canonical section-16 through section-18 rows before string offsets are linked. */
struct Snapshot final {
    std::vector<Squad> squads{};
    std::vector<SquadMember> members{};
    std::vector<SquadAnchor> anchors{};
    /** Exact pre-projection authored spawner-to-rule edges feed SDK declaration metadata. */
    std::uint32_t authoredSpawnerRuleEdgeCount{};
    bool actorLinksComplete{};
    bool ready{};
};

struct GraphSnapshot;

/**
 * Adds the placements a scenario loads that no activity object list carries.
 * A point identity resolves against these only when the object's own lists hold none for that
 * scenario, so no join that already resolves can change.
 */
[[nodiscard]] bool append_external_placements(const topology_inventory::Snapshot& topology,
                                              const external_placements::Index& index,
                                              Facts& facts) noexcept;

/** Reads exact package facts for every object definition in one closed topology. */
[[nodiscard]] bool collect_facts(const topology_inventory::Snapshot& topology,
                                 TagReader reader,
                                 void* readerContext,
                                 Facts& output) noexcept;

/** Links normalized facts to canonical section-16 through section-18 rows. */
[[nodiscard]] bool link(const topology_inventory::Snapshot& topology,
                        const Facts& facts,
                        ActorResolver actorResolver,
                        void* actorContext,
                        Snapshot& output) noexcept;

/** Projects runtime squad rows from one already normalized authored-squad graph. */
[[nodiscard]] bool link(const topology_inventory::Snapshot& topology,
                        const Facts& facts,
                        const GraphSnapshot& graph,
                        ActorResolver actorResolver,
                        void* actorContext,
                        Snapshot& output) noexcept;

/** Runs package extraction and linking without retaining an intermediate copy. */
[[nodiscard]] bool build(const topology_inventory::Snapshot& topology,
                         TagReader reader,
                         void* readerContext,
                         std::span<const SlotSchemaFact> slotSchemas,
                         ActorResolver actorResolver,
                         void* actorContext,
                         Snapshot& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
