#include <Windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstdio>
#include <limits>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../../../middleware/crypto/sha256.h"
#include "activity_sdk_squad_graph.h"
#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_squad_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {
namespace {

namespace crypto = middleware::crypto::sha256;
namespace topology = topology_inventory;

/** One reusable CNG hash operation for the hundreds of thousands of canonical squad ids. */
class SquadIdHasher final {
public:
    /** Opens the reusable CNG hash; the object stays not ready when CNG refuses. */
    SquadIdHasher() noexcept {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
            || BCryptCreateHash(
                   algorithm_, &hash_, nullptr, 0, nullptr, 0, BCRYPT_HASH_REUSABLE_FLAG)
                   < 0) {
            if (algorithm_ != nullptr) {
                BCryptCloseAlgorithmProvider(algorithm_, 0);
            }
            algorithm_ = nullptr;
            hash_ = nullptr;
        }
    }

    ~SquadIdHasher() noexcept {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    SquadIdHasher(const SquadIdHasher&) = delete;
    SquadIdHasher& operator=(const SquadIdHasher&) = delete;

    [[nodiscard]] bool ready() const noexcept {
        return algorithm_ != nullptr && hash_ != nullptr;
    }

    /** Adds one span to the running hash. @return False when the hash is not ready or too long. */
    [[nodiscard]] bool hash(std::span<const std::byte> input, crypto::Digest& output) noexcept {
        return ready() && input.size() <= (std::numeric_limits<ULONG>::max)()
               && BCryptHashData(hash_,
                                 reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data())),
                                 static_cast<ULONG>(input.size()),
                                 0)
                      >= 0
               && BCryptFinishHash(hash_,
                                   reinterpret_cast<PUCHAR>(output.data()),
                                   static_cast<ULONG>(output.size()),
                                   0)
                      >= 0;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};

/** One exact spawner-to-rule edge before scenario rows are emitted. */
struct Edge final {
    const SpawnerFact* spawner{};
    const RuleFact* rule{};
    const DescriptorFact* sourceDescriptor{};
    const DescriptorFact* targetDescriptor{};
    std::string id{};
    std::vector<std::uint32_t> scenarios{};
    bool associationExact{};
};

/** One pending squad keeps child rows owned until canonical sorting finishes. */
struct PendingSquad final {
    Squad row{};
    std::vector<SquadMember> members{};
    std::vector<SquadAnchor> anchors{};
};

/** @return True when one topology text owns exactly its declared bytes. */
[[nodiscard]] bool text_view(const topology::Text& text, std::string_view& output) noexcept {
    output = {};
    if (text.length >= text.value.size()) {
        return false;
    }
    output = std::string_view(text.value.data(), text.length);
    return text.value[text.length] == '\0';
}

/** Formats one bounded identity string. */
template <typename... Values>
[[nodiscard]] bool format_text(std::string& output, const char* pattern, Values... values) {
    std::array<char, 192> bytes{};
    const int count = std::snprintf(bytes.data(), bytes.size(), pattern, values...);
    if (count < 0 || static_cast<std::size_t>(count) >= bytes.size()) {
        return false;
    }
    output.assign(bytes.data(), static_cast<std::size_t>(count));
    return true;
}

/** Appends one unsigned integer in little-endian order. */
template <typename Value>
[[nodiscard]] bool append_little(std::vector<std::byte>& output, Value value) {
    static_assert(std::is_unsigned_v<Value>);
    if (output.size() > (std::numeric_limits<std::size_t>::max)() - sizeof(Value)) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        output.push_back(static_cast<std::byte>((value >> (index * 8U)) & Value{0xFFU}));
    }
    return true;
}

/** Appends one length-prefixed byte string to a domain hash input. */
[[nodiscard]] bool append_hash_part(std::vector<std::byte>& output, std::string_view value) {
    if (!append_little(output, static_cast<std::uint64_t>(value.size()))
        || value.size() > (std::numeric_limits<std::size_t>::max)() - output.size()) {
        return false;
    }
    output.reserve(output.size() + value.size());
    for (const unsigned char byte : value) {
        output.push_back(static_cast<std::byte>(byte));
    }
    return true;
}

/** Replays the SDK builder's typed SHA-256 domain hash. */
[[nodiscard]] bool domain_hash(SquadIdHasher& hasher,
                               std::string_view domain,
                               std::span<const std::string_view> parts,
                               std::string& output) {
    output.clear();
    std::vector<std::byte> input{};
    try {
        if (!append_little(input, static_cast<std::uint32_t>(domain.size()))) {
            return false;
        }
        input.reserve(input.size() + domain.size());
        for (const unsigned char byte : domain) {
            input.push_back(static_cast<std::byte>(byte));
        }
        for (const std::string_view part : parts) {
            if (!append_hash_part(input, part)) {
                return false;
            }
        }
    } catch (...) {
        return false;
    }
    crypto::Digest digest{};
    if (!hasher.hash(input, digest)) {
        return false;
    }
    // Squad ids are published in lowercase hex.
    static constexpr std::array<char, 16> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    output.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = static_cast<unsigned char>(digest[index]);
        output[index * 2U] = kHex[value >> 4U];
        output[index * 2U + 1U] = kHex[value & 0xFU];
    }
    return true;
}

/** One scenario and exact placed-entry identity lookup key. */
struct PlacementKey final {
    std::uint32_t scenarioIndex{};
    std::uint64_t identity{};

    bool operator==(const PlacementKey&) const = default;
};

/** Hashes one occurrence-local placement lookup key. */
struct PlacementKeyHash final {
    [[nodiscard]] std::size_t operator()(const PlacementKey& value) const noexcept {
        const std::uint64_t mixed = value.identity ^ (value.identity >> 32U)
                                    ^ (static_cast<std::uint64_t>(value.scenarioIndex) << 13U);
        return static_cast<std::size_t>(mixed);
    }
};

/** One authored config's occurrences for a specific world object and scenario. */
struct ConfigOccurrenceKey final {
    std::uint32_t configTag{};
    std::uint32_t objectIndex{};
    std::uint32_t scenarioIndex{};

    bool operator==(const ConfigOccurrenceKey&) const = default;
};

struct ConfigOccurrenceKeyHash final {
    [[nodiscard]] std::size_t operator()(const ConfigOccurrenceKey& value) const noexcept {
        return static_cast<std::size_t>(value.configTag)
               ^ (static_cast<std::size_t>(value.objectIndex) << 11U)
               ^ (static_cast<std::size_t>(value.scenarioIndex) << 23U);
    }
};

/** Counts duplicate uses of one config in one occurrence without rescanning its config bucket. */
struct ConfigOccurrenceCountKey final {
    std::uint32_t configTag{};
    std::uint32_t occurrenceIndex{};

    bool operator==(const ConfigOccurrenceCountKey&) const = default;
};

struct ConfigOccurrenceCountKeyHash final {
    [[nodiscard]] std::size_t operator()(const ConfigOccurrenceCountKey& value) const noexcept {
        return static_cast<std::size_t>(value.configTag)
               ^ (static_cast<std::size_t>(value.occurrenceIndex) << 17U);
    }
};

/** Tests one output squad's natural identity before child ranges are assigned. */
[[nodiscard]] auto squad_natural(const PendingSquad& value) noexcept {
    return std::tuple(value.row.scenarioIndex,
                      value.row.occurrenceIndex,
                      value.row.objectIndex,
                      value.row.slotIndex,
                      value.row.spawnerConfigTag,
                      value.row.spawnRuleConfigTag);
}

// Squads whose spawner names no rule: emitted without a rule or anchors, placeable only with a
// selected type-66 rule slot. Shares the linker's private identity and member helpers.

// Match the full descriptor tuple; no schema guesses based on slot type alone.
[[nodiscard]] bool unbound_descriptor_exact(
    const topology::Snapshot& topology,
    const std::unordered_map<std::uint32_t, const GraphSlotSchemaProvenance*>& schemas,
    const GraphDescriptor& d) {
    if (!d.complete || d.slotIndex >= topology.slots.size()
        || d.objectIndex >= topology.objects.size()) {
        return false;
    }
    const auto& slot = topology.slots[d.slotIndex];
    if (slot.objectIndex != d.objectIndex || slot.slotType != format::kSquadSlotType) {
        return false;
    }
    const auto found = schemas.find(d.slotIndex);
    const auto* match = found == schemas.end() ? nullptr : found->second;
    return match != nullptr && match->exact && match->componentClass == d.componentClass
           && match->senseSchema == d.senseSchema && match->authSchema == d.authSchema;
}

// A repeated authored config path is ambiguity, even when all paths have equal coordinates.
[[nodiscard]] bool unbound_occurrence_exact(const topology::Snapshot& topology,
                                            const std::vector<const GraphConfigContext*>& contexts,
                                            const GraphDescriptor& d,
                                            std::uint32_t occurrenceIndex) {
    if (occurrenceIndex >= topology.occurrences.size()) {
        return false;
    }
    const auto& o = topology.occurrences[occurrenceIndex];
    if (o.objectIndex != d.objectIndex) {
        return false;
    }
    std::uint32_t count = 0;
    bool exact = false;
    for (const auto* cp : contexts) {
        const auto& c = *cp;
        if (c.configTag != d.configTag || c.occurrenceIndex != occurrenceIndex) {
            continue;
        }
        ++count;
        exact = c.complete && c.objectIndex == d.objectIndex && c.scenarioIndex == o.scenarioIndex;
    }
    return count == 1 && exact;
}

// Append independently owned sources. Never construct or modify a GraphEdge.
[[nodiscard]] bool project_unbound_squads(const topology::Snapshot& topology,
                                          const Facts& facts,
                                          const GraphSnapshot& graph,
                                          SquadIdHasher& hasher,
                                          ActorResolver actorResolver,
                                          void* actorContext,
                                          std::vector<PendingSquad>& pending) {
    std::unordered_map<std::uint32_t, const GraphSlotSchemaProvenance*> schemas{};
    for (const auto& schema : graph.slotSchemas) {
        if (!schemas.emplace(schema.slotIndex, &schema).second) {
            return false;
        }
    }
    std::unordered_map<std::uint32_t, std::size_t> spawnersByConfig{};
    std::unordered_map<std::uint32_t, std::vector<const GraphConfigContext*>> contextsByConfig{};
    for (std::size_t i = 0; i < graph.spawners.size(); ++i) {
        if (!spawnersByConfig.emplace(graph.spawners[i].configTag, i).second) {
            return false;
        }
    }
    for (const auto& c : graph.configContexts) {
        contextsByConfig[c.configTag].push_back(&c);
    }
    for (const auto& d : graph.descriptors) {
        const auto foundSource = spawnersByConfig.find(d.configTag);
        if (foundSource == spawnersByConfig.end()) {
            continue;
        }
        const auto i = foundSource->second;
        if (i >= facts.spawners.size()) {
            return false;
        }
        const GraphSpawner& spawner = graph.spawners[i];
        const SpawnerFact& source = facts.spawners[i];
        if (!spawner.requiresSelectedRule) {
            continue;
        }
        if (spawner.configTag != source.configTag) {
            return false;
        }
        if (d.componentClass != format::kSquadComponentClass
            || d.senseSchema != format::kSquadSenseSchema
            || d.authSchema != format::kSquadAuthSchema
            || !unbound_descriptor_exact(topology, schemas, d)) {
            continue;
        }
        const auto foundContexts = contextsByConfig.find(d.configTag);
        if (foundContexts == contextsByConfig.end()) {
            continue;
        }
        std::vector<std::uint32_t> occurrenceIndexes{};
        for (const auto* c : foundContexts->second) {
            occurrenceIndexes.push_back(c->occurrenceIndex);
        }
        std::sort(occurrenceIndexes.begin(), occurrenceIndexes.end());
        occurrenceIndexes.erase(std::unique(occurrenceIndexes.begin(), occurrenceIndexes.end()),
                                occurrenceIndexes.end());
        for (const auto oi : occurrenceIndexes) {
            if (!unbound_occurrence_exact(topology, foundContexts->second, d, oi)) {
                continue;
            }
            const auto& occurrence = topology.occurrences[oi];
            std::string_view occurrenceId{};
            if (!text_view(occurrence.id, occurrenceId)) {
                return false;
            }
            const std::array<std::string_view, 2> parts{d.id, occurrenceId};
            std::string digest{};
            if (!domain_hash(hasher, "sunrise-runtime-unbound-squad-v1", parts, digest)) {
                return false;
            }
            PendingSquad squad{};
            squad.row.id = "squad/" + digest;
            squad.row.scenarioIndex = occurrence.scenarioIndex;
            squad.row.objectIndex = d.objectIndex;
            squad.row.slotIndex = d.slotIndex;
            squad.row.spawnerConfigTag = d.configTag;
            squad.row.spawnRuleConfigTag = format::kAbsentIndex;
            squad.row.occurrenceIndex = oi;
            squad.row.flags = format::kSquadRequiresSelectedRule
                              | format::kSquadSourceDescriptorExact
                              | format::kSquadScenarioOccurrenceExact;
            bool sourceActorLinksComplete = true;
            for (std::uint32_t mi = 0; mi < source.members.size(); ++mi) {
                SquadMember member{};
                if (!detail::build_member(source.members[mi],
                                          digest,
                                          mi,
                                          actorResolver,
                                          actorContext,
                                          member,
                                          sourceActorLinksComplete)) {
                    return false;
                }
                squad.members.push_back(std::move(member));
            }
            // An unresolved unbound source must not disable the authored bound squads.
            if (!sourceActorLinksComplete) {
                continue;
            }
            const bool countValid = squad.members.size() >= format::kSquadMinimumMemberCount
                                    && squad.members.size() <= format::kSquadMaximumMemberCount;
            if (countValid) {
                squad.row.flags |= format::kSquadMemberCountValid;
            }
            if (countValid
                && std::all_of(
                    squad.members.begin(), squad.members.end(), [](const SquadMember& m) {
                        return (m.flags & format::kSquadMemberInvariantReadyMask)
                                   == format::kSquadMemberInvariantReadyMask
                               && m.defaultCount > 0;
                    })) {
                squad.row.flags |= format::kSquadCandidateCountsInvariantComplete;
            }
            std::vector<std::uint32_t> keys{};
            for (const auto& m : squad.members) {
                keys.push_back(m.memberKey);
            }
            std::sort(keys.begin(), keys.end());
            if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
                return false;
            }
            // Empty anchors and an absent rule are intentional; kSquadRunnableMask cannot pass.
            pending.push_back(std::move(squad));
        }
    }
    return true;
}

} // namespace

/** Normalizes the authored graph once, then derives the narrower runnable squad projection. */
bool link(const topology::Snapshot& topology,
          const Facts& facts,
          ActorResolver actorResolver,
          void* actorContext,
          Snapshot& output) noexcept {
    GraphSnapshot graph{};
    if (!build_graph(topology, facts, graph)) {
        output = {};
        return false;
    }
    return link(topology, facts, graph, actorResolver, actorContext, output);
}

/** Links normalized facts to canonical section-16 through section-18 rows. */
bool link(const topology::Snapshot& topology,
          const Facts& facts,
          const GraphSnapshot& graph,
          ActorResolver actorResolver,
          void* actorContext,
          Snapshot& output) noexcept {
    output = {};
    if (!detail::valid_topology(topology) || !graph.ready) {
        return false;
    }
    try {
        if (!detail::validate_facts(topology, facts)
            || graph.spawners.size() != facts.spawners.size()
            || graph.rules.size() != facts.rules.size()
            || graph.descriptors.size() != facts.descriptors.size()
            || graph.slotSchemas.size() != facts.slotSchemas.size()
            || graph.configContexts.size() != facts.configOccurrences.size()
            || graph.placementContexts.size() != facts.placementOccurrences.size()) {
            return false;
        }

        SquadIdHasher squadIdHasher{};
        if (!squadIdHasher.ready()) {
            return false;
        }

        std::unordered_map<ConfigOccurrenceKey, std::vector<std::uint32_t>, ConfigOccurrenceKeyHash>
            occurrencesByConfigObjectScenario{};
        std::unordered_map<ConfigOccurrenceCountKey, std::uint32_t, ConfigOccurrenceCountKeyHash>
            configOccurrenceCounts{};
        occurrencesByConfigObjectScenario.reserve(graph.configContexts.size());
        configOccurrenceCounts.reserve(graph.configContexts.size());
        for (const GraphConfigContext& context : graph.configContexts) {
            if (context.occurrenceIndex >= topology.occurrences.size()) {
                return false;
            }
            const topology::Occurrence& occurrence = topology.occurrences[context.occurrenceIndex];
            std::string_view occurrenceId{};
            if (!text_view(occurrence.id, occurrenceId)) {
                return false;
            }
            occurrencesByConfigObjectScenario[{context.configTag,
                                               occurrence.objectIndex,
                                               occurrence.scenarioIndex}]
                .push_back(context.occurrenceIndex);
            ++configOccurrenceCounts[{context.configTag, context.occurrenceIndex}];
        }
        for (auto& [key, occurrences] : occurrencesByConfigObjectScenario) {
            (void)key;
            std::sort(occurrences.begin(), occurrences.end(), [&topology](auto first, auto second) {
                std::string_view firstId{};
                std::string_view secondId{};
                return text_view(topology.occurrences[first].id, firstId)
                       && text_view(topology.occurrences[second].id, secondId)
                       && firstId < secondId;
            });
            occurrences.erase(std::unique(occurrences.begin(), occurrences.end()),
                              occurrences.end());
        }
        std::
            unordered_map<PlacementKey, std::vector<const GraphPlacementContext*>, PlacementKeyHash>
                placementsByIdentity{};
        for (const GraphPlacementContext& placement : graph.placementContexts) {
            placementsByIdentity[{placement.scenarioIndex, placement.placedEntryIdentity}]
                .push_back(&placement);
        }
        std::unordered_map<std::uint32_t, const GraphSlotSchemaProvenance*> schemasBySlot{};
        for (const GraphSlotSchemaProvenance& schema : graph.slotSchemas) {
            schemasBySlot.emplace(schema.slotIndex, &schema);
        }

        std::vector<Edge> edges{};
        edges.reserve(graph.edges.size());
        for (std::uint32_t graphEdgeRow = 0; graphEdgeRow < graph.edges.size(); ++graphEdgeRow) {
            const GraphEdge& source = graph.edges[graphEdgeRow];
            if (source.spawnerRow >= facts.spawners.size() || source.ruleRow >= facts.rules.size()
                || source.sourceDescriptorRow >= facts.descriptors.size()
                || source.targetDescriptorRow >= facts.descriptors.size()
                || source.scenarioContexts.first > graph.edgeContexts.size()
                || source.scenarioContexts.count
                       > graph.edgeContexts.size() - source.scenarioContexts.first) {
                return false;
            }
            Edge edge{};
            edge.spawner = &facts.spawners[source.spawnerRow];
            edge.rule = &facts.rules[source.ruleRow];
            edge.sourceDescriptor = &facts.descriptors[source.sourceDescriptorRow];
            edge.targetDescriptor = &facts.descriptors[source.targetDescriptorRow];
            edge.id = source.id;
            edge.associationExact = source.associationExact;
            for (std::uint32_t contextRow = source.scenarioContexts.first;
                 contextRow < source.scenarioContexts.first + source.scenarioContexts.count;
                 ++contextRow) {
                const GraphEdgeContext& context = graph.edgeContexts[contextRow];
                if (context.edgeRow != graphEdgeRow
                    || context.scenarioIndex >= topology.scenarios.size()) {
                    return false;
                }
                edge.scenarios.push_back(context.scenarioIndex);
            }
            edges.push_back(std::move(edge));
        }

        if (edges.size() >= format::kAbsentIndex) {
            return false;
        }
        const std::uint32_t authoredSpawnerRuleEdgeCount = static_cast<std::uint32_t>(edges.size());
        std::vector<PendingSquad> pending{};
        bool actorLinksComplete = true;
        for (const Edge& edge : edges) {
            if (edge.sourceDescriptor->objectIndex != edge.targetDescriptor->objectIndex) {
                continue;
            }
            for (const std::uint32_t scenarioIndex : edge.scenarios) {
                const auto sourceOccurrences = occurrencesByConfigObjectScenario.find(
                    {edge.spawner->configTag, edge.sourceDescriptor->objectIndex, scenarioIndex});
                if (sourceOccurrences == occurrencesByConfigObjectScenario.end()) {
                    continue;
                }
                const std::vector<std::uint32_t>& occurrenceIndexes = sourceOccurrences->second;
                for (const std::uint32_t occurrenceIndex : occurrenceIndexes) {
                    const topology::Occurrence& occurrence = topology.occurrences[occurrenceIndex];
                    std::string_view occurrenceId{};
                    if (!text_view(occurrence.id, occurrenceId)) {
                        return false;
                    }
                    std::string squadDigest{};
                    const std::array<std::string_view, 2> parts{edge.id, occurrenceId};
                    if (!domain_hash(
                            squadIdHasher, "sunrise-runtime-squad-v1", parts, squadDigest)) {
                        return false;
                    }
                    PendingSquad squad{};
                    squad.row.id = "squad/" + squadDigest;
                    squad.row.scenarioIndex = scenarioIndex;
                    squad.row.objectIndex = edge.sourceDescriptor->objectIndex;
                    squad.row.slotIndex = edge.sourceDescriptor->slotIndex;
                    squad.row.spawnerConfigTag = edge.spawner->configTag;
                    squad.row.spawnRuleConfigTag = edge.rule->configTag;
                    squad.row.occurrenceIndex = occurrenceIndex;

                    bool sourceExact = false;
                    const auto schema = schemasBySlot.find(edge.sourceDescriptor->slotIndex);
                    if (schema != schemasBySlot.end() && schema->second->exact) {
                        const GraphSlotSchemaProvenance& slotSchema = *schema->second;
                        sourceExact = topology.slots[edge.sourceDescriptor->slotIndex].objectIndex
                                          == edge.sourceDescriptor->objectIndex
                                      && topology.slots[edge.sourceDescriptor->slotIndex].slotType
                                             == format::kSquadSlotType
                                      && slotSchema.componentClass == format::kSquadComponentClass
                                      && slotSchema.senseSchema == format::kSquadSenseSchema
                                      && slotSchema.authSchema == format::kSquadAuthSchema;
                    }
                    if (sourceExact) {
                        squad.row.flags |= format::kSquadSourceDescriptorExact;
                    }
                    if (edge.associationExact) {
                        squad.row.flags |= format::kSquadSpawnerRuleEdgeExact;
                    }
                    squad.row.flags |= format::kSquadScenarioOccurrenceExact;

                    squad.members.reserve(edge.spawner->members.size());
                    for (std::uint32_t memberOrdinal = 0;
                         memberOrdinal < edge.spawner->members.size();
                         ++memberOrdinal) {
                        SquadMember member{};
                        if (!detail::build_member(edge.spawner->members[memberOrdinal],
                                                  squadDigest,
                                                  memberOrdinal,
                                                  actorResolver,
                                                  actorContext,
                                                  member,
                                                  actorLinksComplete)) {
                            return false;
                        }
                        squad.members.push_back(std::move(member));
                    }

                    const auto ruleContextCount =
                        configOccurrenceCounts.find({edge.rule->configTag, occurrenceIndex});
                    const std::size_t exactRuleContextCount =
                        ruleContextCount == configOccurrenceCounts.end() ? 0
                                                                         : ruleContextCount->second;
                    std::vector<SquadAnchor> exactAnchors{};
                    if (edge.rule->inlineForm && exactRuleContextCount == 1
                        && edge.rule->points.size() == 1) {
                        // The one inline point names the spawner's own placement.
                        const CandidateFact& placement = edge.spawner->inlinePlacement;
                        const RulePointFact& point = edge.rule->points.front();
                        if (placement.state == CandidateState::exactPlacement
                            && placement.placedEntryIdentity == point.placedEntryIdentity
                            && placement.placedEntryIdentity != 0
                            && placement.placedEntryIdentity
                                   != (std::numeric_limits<std::uint64_t>::max)()) {
                            SquadAnchor anchor{};
                            if (!format_text(anchor.id,
                                             "squad-anchor/%.*s/%06x",
                                             static_cast<int>(squadDigest.size()),
                                             squadDigest.data(),
                                             0U)) {
                                return false;
                            }
                            anchor.pointOrdinal = 0;
                            anchor.objectListTag = 0;
                            anchor.placementOrdinal = 0;
                            anchor.flags = format::kSquadAnchorExact | format::kSquadAnchorInline;
                            anchor.placedEntryIdentity = placement.placedEntryIdentity;
                            anchor.positionBits = placement.positionBits;
                            exactAnchors.push_back(std::move(anchor));
                        }
                    } else if (!edge.rule->inlineForm && exactRuleContextCount == 1
                               && !edge.rule->points.empty()) {
                        exactAnchors.reserve(edge.rule->points.size());
                        for (std::uint32_t pointOrdinal = 0;
                             pointOrdinal < edge.rule->points.size();
                             ++pointOrdinal) {
                            const RulePointFact& point = edge.rule->points[pointOrdinal];
                            const auto matches = placementsByIdentity.find(
                                {scenarioIndex, point.placedEntryIdentity});
                            if (matches == placementsByIdentity.end()
                                || matches->second.size() != 1) {
                                exactAnchors.clear();
                                break;
                            }
                            const GraphPlacementContext& placement = *matches->second.front();
                            // An external row is not owned by this occurrence, and a
                            // descriptor-embedded one names no object list. The client resolves
                            // either by scanning the objects the scenario loads.
                            const bool ownedHere =
                                placement.external
                                || (placement.occurrenceIndex == occurrenceIndex
                                    && placement.objectListTag != 0
                                    && placement.objectListTag != format::kAbsentIndex);
                            if (!ownedHere || placement.placedEntryIdentity == 0
                                || placement.placedEntryIdentity
                                       == (std::numeric_limits<std::uint64_t>::max)()) {
                                exactAnchors.clear();
                                break;
                            }
                            SquadAnchor anchor{};
                            if (!format_text(anchor.id,
                                             "squad-anchor/%.*s/%06x",
                                             static_cast<int>(squadDigest.size()),
                                             squadDigest.data(),
                                             static_cast<unsigned>(pointOrdinal))) {
                                return false;
                            }
                            anchor.pointOrdinal = pointOrdinal;
                            anchor.objectListTag = placement.objectListTag;
                            anchor.placementOrdinal = placement.placementOrdinal;
                            anchor.flags =
                                format::kSquadAnchorExact
                                | (placement.external ? format::kSquadAnchorExternal : 0U);
                            anchor.placedEntryIdentity = placement.placedEntryIdentity;
                            anchor.positionBits = placement.positionBits;
                            exactAnchors.push_back(std::move(anchor));
                        }
                    }
                    if (exactAnchors.size() == edge.rule->points.size()
                        && !edge.rule->points.empty()) {
                        squad.anchors = std::move(exactAnchors);
                        squad.row.flags |= format::kSquadAllPointsExact;
                    }

                    const bool memberCountValid =
                        squad.members.size() >= format::kSquadMinimumMemberCount
                        && squad.members.size() <= format::kSquadMaximumMemberCount;
                    if (memberCountValid) {
                        squad.row.flags |= format::kSquadMemberCountValid;
                    }
                    const bool invariantComplete =
                        memberCountValid
                        && std::all_of(squad.members.begin(),
                                       squad.members.end(),
                                       [](const SquadMember& member) {
                                           return (member.flags
                                                   & format::kSquadMemberInvariantReadyMask)
                                                      == format::kSquadMemberInvariantReadyMask
                                                  && member.defaultCount > 0;
                                       });
                    if (invariantComplete) {
                        squad.row.flags |= format::kSquadCandidateCountsInvariantComplete;
                    }
                    std::vector<std::uint32_t> memberKeys{};
                    memberKeys.reserve(squad.members.size());
                    for (const SquadMember& member : squad.members) {
                        memberKeys.push_back(member.memberKey);
                    }
                    std::sort(memberKeys.begin(), memberKeys.end());
                    if (std::adjacent_find(memberKeys.begin(), memberKeys.end())
                        != memberKeys.end()) {
                        return false;
                    }
                    std::vector<std::uint64_t> anchorIdentities{};
                    anchorIdentities.reserve(squad.anchors.size());
                    for (const SquadAnchor& anchor : squad.anchors) {
                        anchorIdentities.push_back(anchor.placedEntryIdentity);
                    }
                    std::sort(anchorIdentities.begin(), anchorIdentities.end());
                    if (std::adjacent_find(anchorIdentities.begin(), anchorIdentities.end())
                        != anchorIdentities.end()) {
                        return false;
                    }
                    pending.push_back(std::move(squad));
                }
            }
        }

        if (!project_unbound_squads(
                topology, facts, graph, squadIdHasher, actorResolver, actorContext, pending)) {
            return false;
        }

        std::sort(pending.begin(),
                  pending.end(),
                  [](const PendingSquad& first, const PendingSquad& second) {
                      const auto firstNatural = squad_natural(first);
                      const auto secondNatural = squad_natural(second);
                      return firstNatural != secondNatural ? firstNatural < secondNatural
                                                           : first.row.id < second.row.id;
                  });
        for (std::size_t index = 1; index < pending.size(); ++index) {
            if (squad_natural(pending[index - 1]) == squad_natural(pending[index])) {
                return false;
            }
        }
        if (pending.size() >= format::kAbsentIndex) {
            return false;
        }
        Snapshot linked{};
        linked.squads.reserve(pending.size());
        for (std::uint32_t squadIndex = 0; squadIndex < pending.size(); ++squadIndex) {
            PendingSquad& source = pending[squadIndex];
            if (linked.members.size() >= format::kAbsentIndex
                || linked.anchors.size() >= format::kAbsentIndex
                || source.members.size() > format::kAbsentIndex - linked.members.size()
                || source.anchors.size() > format::kAbsentIndex - linked.anchors.size()) {
                return false;
            }
            source.row.members = {static_cast<std::uint32_t>(linked.members.size()),
                                  static_cast<std::uint32_t>(source.members.size())};
            for (SquadMember& member : source.members) {
                member.squadIndex = squadIndex;
                linked.members.push_back(std::move(member));
            }
            source.row.anchors = {static_cast<std::uint32_t>(linked.anchors.size()),
                                  static_cast<std::uint32_t>(source.anchors.size())};
            for (SquadAnchor& anchor : source.anchors) {
                anchor.squadIndex = squadIndex;
                linked.anchors.push_back(std::move(anchor));
            }
            linked.squads.push_back(std::move(source.row));
        }
        linked.authoredSpawnerRuleEdgeCount = authoredSpawnerRuleEdgeCount;
        linked.actorLinksComplete = actorLinksComplete;
        linked.ready = true;
        output = std::move(linked);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
