#include "activity_sdk_squad_graph.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "activity_sdk_squad_graph_internal.h"
#include "activity_sdk_squad_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {
namespace {

namespace topology = topology_inventory;

/** Formats one bounded canonical identity. */
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

/** Inserts one unique config tag and rejects duplicate normalized definitions. */
template <typename Row>
[[nodiscard]] bool insert_config(std::unordered_map<std::uint32_t, std::uint32_t>& rows,
                                 const Row& row,
                                 std::uint32_t index) {
    return rows.emplace(row.configTag, index).second;
}

/** Flattens global authored spawner, member, and all six candidate-lane definitions. */
[[nodiscard]] bool build_spawners(const Facts& facts,
                                  GraphSnapshot& output,
                                  std::unordered_map<std::uint32_t, std::uint32_t>& byConfig) {
    output.spawners.reserve(facts.spawners.size());
    for (std::uint32_t spawnerRow = 0; spawnerRow < facts.spawners.size(); ++spawnerRow) {
        const SpawnerFact& source = facts.spawners[spawnerRow];
        GraphSpawner row{};
        if (!format_text(
                row.id, "authored-spawner/%08x", static_cast<unsigned>(source.configTag))) {
            return false;
        }
        row.configTag = source.configTag;
        row.rawReference98 = source.rawReference98;
        row.rawReferenceA0 = source.rawReferenceA0;
        row.primaryComponentOffset = source.primaryComponentOffset;
        row.secondaryComponentOffset = source.secondaryComponentOffset;
        row.primaryComponentClass = source.primaryComponentClass;
        row.secondaryComponentClass = source.secondaryComponentClass;
        row.members.first = static_cast<std::uint32_t>(output.members.size());
        row.members.count = static_cast<std::uint32_t>(source.members.size());
        row.complete = source.complete;
        row.hasInlinePointSet = source.hasInlinePointSet;
        row.requiresSelectedRule = requires_selected_spawn_rule(source.rawReference98,
                                                                source.rawReferenceA0,
                                                                source.complete,
                                                                source.inlinePointSetInspected,
                                                                source.hasInlinePointSet);
        if (!insert_config(byConfig, row, spawnerRow)) {
            return false;
        }
        for (std::uint32_t memberOrdinal = 0; memberOrdinal < source.members.size();
             ++memberOrdinal) {
            const MemberFact& sourceMember = source.members[memberOrdinal];
            GraphMember member{};
            if (!format_text(member.id,
                             "authored-spawner-member/%08x/%06x",
                             static_cast<unsigned>(source.configTag),
                             static_cast<unsigned>(memberOrdinal))) {
                return false;
            }
            member.spawnerRow = spawnerRow;
            member.memberOrdinal = memberOrdinal;
            member.memberKey = sourceMember.memberKey;
            member.reservedU32 = sourceMember.reservedU32;
            const std::uint32_t memberRow = static_cast<std::uint32_t>(output.members.size());
            for (std::uint32_t lane = 0; lane < sourceMember.candidates.size(); ++lane) {
                const auto& sourceLane = sourceMember.candidates[lane];
                member.candidateLanes[lane].first =
                    static_cast<std::uint32_t>(output.candidates.size());
                member.candidateLanes[lane].count = static_cast<std::uint32_t>(sourceLane.size());
                for (std::uint32_t candidateOrdinal = 0; candidateOrdinal < sourceLane.size();
                     ++candidateOrdinal) {
                    const CandidateFact& sourceCandidate = sourceLane[candidateOrdinal];
                    GraphCandidate candidate{};
                    if (!format_text(candidate.id,
                                     "authored-spawner-candidate/%08x/%06x/%02x/%06x",
                                     static_cast<unsigned>(source.configTag),
                                     static_cast<unsigned>(memberOrdinal),
                                     static_cast<unsigned>(lane),
                                     static_cast<unsigned>(candidateOrdinal))) {
                        return false;
                    }
                    candidate.spawnerRow = spawnerRow;
                    candidate.memberRow = memberRow;
                    candidate.memberOrdinal = memberOrdinal;
                    candidate.lane = lane;
                    candidate.candidateOrdinal = candidateOrdinal;
                    candidate.actorDefinitionTag = sourceCandidate.actorDefinitionTag;
                    candidate.state = sourceCandidate.state;
                    candidate.candidateDescriptorOffset = sourceCandidate.candidateDescriptorOffset;
                    candidate.placementRelative = sourceCandidate.placementRelative;
                    candidate.placementOffset = sourceCandidate.placementOffset;
                    candidate.candidateTail = sourceCandidate.candidateTail;
                    candidate.placedEntryClass = sourceCandidate.placedEntryClass;
                    candidate.quaternionBits = sourceCandidate.quaternionBits;
                    candidate.positionBits = sourceCandidate.positionBits;
                    candidate.uniformScaleBits = sourceCandidate.uniformScaleBits;
                    candidate.nameHash = sourceCandidate.nameHash;
                    candidate.placementFlagsRaw = sourceCandidate.placementFlagsRaw;
                    candidate.placedEntryIdentity = sourceCandidate.placedEntryIdentity;
                    output.candidates.push_back(std::move(candidate));
                }
            }
            output.members.push_back(std::move(member));
        }
        output.spawners.push_back(std::move(row));
    }
    return true;
}

/** Flattens global authored spawn-rule and exact 0x48 point definitions. */
[[nodiscard]] bool build_rules(const Facts& facts,
                               GraphSnapshot& output,
                               std::unordered_map<std::uint32_t, std::uint32_t>& byConfig) {
    output.rules.reserve(facts.rules.size());
    for (std::uint32_t ruleRow = 0; ruleRow < facts.rules.size(); ++ruleRow) {
        const RuleFact& source = facts.rules[ruleRow];
        GraphRule row{};
        if (!format_text(row.id,
                         source.inlineForm ? "authored-spawn-rule/%08x/inline"
                                           : "authored-spawn-rule/%08x",
                         static_cast<unsigned>(source.configTag))) {
            return false;
        }
        row.configTag = source.configTag;
        row.inlineForm = source.inlineForm;
        row.primaryComponentOffset = source.primaryComponentOffset;
        row.secondaryComponentOffset = source.secondaryComponentOffset;
        row.primaryComponentClass = source.primaryComponentClass;
        row.secondaryComponentClass = source.secondaryComponentClass;
        row.points.first = static_cast<std::uint32_t>(output.points.size());
        row.points.count = static_cast<std::uint32_t>(source.points.size());
        row.complete = source.complete;
        if (!insert_config(byConfig, row, ruleRow)) {
            return false;
        }
        for (std::uint32_t pointOrdinal = 0; pointOrdinal < source.points.size(); ++pointOrdinal) {
            const RulePointFact& sourcePoint = source.points[pointOrdinal];
            GraphPoint point{};
            if (!format_text(point.id,
                             "authored-spawn-point/%08x/%06x",
                             static_cast<unsigned>(source.configTag),
                             static_cast<unsigned>(pointOrdinal))) {
                return false;
            }
            point.ruleRow = ruleRow;
            point.pointOrdinal = pointOrdinal;
            point.placedEntryIdentity = sourcePoint.placedEntryIdentity;
            point.rowOffset = sourcePoint.rowOffset;
            point.rawTail = sourcePoint.rawTail;
            output.points.push_back(std::move(point));
        }
        output.rules.push_back(std::move(row));
    }
    return true;
}

/** Pairs every inline rule with the spawner that owns it. Both sides must agree. */
[[nodiscard]] bool
link_inline_rules(GraphSnapshot& output,
                  const std::unordered_map<std::uint32_t, std::uint32_t>& spawnersByConfig) {
    for (std::uint32_t ruleRow = 0; ruleRow < output.rules.size(); ++ruleRow) {
        GraphRule& rule = output.rules[ruleRow];
        if (!rule.inlineForm) {
            continue;
        }
        const auto spawner = spawnersByConfig.find(rule.configTag);
        if (spawner == spawnersByConfig.end()
            || !output.spawners[spawner->second].hasInlinePointSet) {
            return false;
        }
        rule.spawnerRow = spawner->second;
        output.spawners[spawner->second].inlineRuleRow = ruleRow;
    }
    return std::all_of(output.spawners.begin(), output.spawners.end(), [](const auto& spawner) {
        return spawner.hasInlinePointSet == (spawner.inlineRuleRow != format::kAbsentIndex);
    });
}

/** Copies every descriptor and raw provenance input in canonical source order. */
[[nodiscard]] bool build_provenance(const Facts& facts, GraphSnapshot& output) {
    output.descriptors.reserve(facts.descriptors.size());
    for (const DescriptorFact& source : facts.descriptors) {
        output.descriptors.push_back({source.id,
                                      source.configTag,
                                      source.objectIndex,
                                      source.slotIndex,
                                      source.descriptorOffset,
                                      source.componentClass,
                                      source.senseSchema,
                                      source.authSchema,
                                      source.complete});
    }
    output.slotSchemas.reserve(facts.slotSchemas.size());
    for (const SlotSchemaFact& source : facts.slotSchemas) {
        output.slotSchemas.push_back({source.slotIndex,
                                      source.componentClass,
                                      source.senseSchema,
                                      source.authSchema,
                                      source.exact});
    }
    output.actorDefinitions.reserve(facts.actorDefinitionTags.size());
    for (const std::uint32_t tag : facts.actorDefinitionTags) {
        output.actorDefinitions.push_back({tag});
    }
    return true;
}

/** Copies scenario-owned config and placement contexts with stable global row identities. */
[[nodiscard]] bool
build_occurrence_contexts(const topology::Snapshot& topology,
                          const Facts& facts,
                          const std::unordered_map<std::uint32_t, std::uint32_t>& spawnersByConfig,
                          const std::unordered_map<std::uint32_t, std::uint32_t>& rulesByConfig,
                          GraphSnapshot& output) {
    output.configContexts.reserve(facts.configOccurrences.size());
    for (std::uint32_t globalRow = 0; globalRow < facts.configOccurrences.size(); ++globalRow) {
        const ConfigOccurrenceFact& source = facts.configOccurrences[globalRow];
        if (source.occurrenceIndex >= topology.occurrences.size()) {
            return false;
        }
        GraphConfigContext row{};
        row.id = source.id;
        row.globalRow = globalRow;
        row.scenarioIndex = topology.occurrences[source.occurrenceIndex].scenarioIndex;
        row.configTag = source.configTag;
        row.occurrenceIndex = source.occurrenceIndex;
        row.objectIndex = source.objectIndex;
        row.pathOrdinal = source.pathOrdinal;
        row.declaredBubbleIndex = source.declaredBubbleIndex;
        const auto spawner = spawnersByConfig.find(source.configTag);
        row.spawnerRow = spawner == spawnersByConfig.end() ? format::kAbsentIndex : spawner->second;
        const auto rule = rulesByConfig.find(source.configTag);
        row.ruleRow = rule == rulesByConfig.end() ? format::kAbsentIndex : rule->second;
        row.complete = source.complete;
        output.configContexts.push_back(std::move(row));
    }
    output.placementContexts.reserve(facts.placementOccurrences.size());
    for (std::uint32_t globalRow = 0; globalRow < facts.placementOccurrences.size(); ++globalRow) {
        const PlacementOccurrenceFact& source = facts.placementOccurrences[globalRow];
        if (source.external ? source.scenarioIndex >= topology.scenarios.size()
                            : source.occurrenceIndex >= topology.occurrences.size()) {
            return false;
        }
        GraphPlacementContext row{};
        row.id = source.id;
        row.globalRow = globalRow;
        row.external = source.external;
        row.scenarioIndex = source.external
                                ? source.scenarioIndex
                                : topology.occurrences[source.occurrenceIndex].scenarioIndex;
        row.occurrenceIndex = source.occurrenceIndex;
        row.objectListTag = source.objectListTag;
        row.placementOrdinal = source.placementOrdinal;
        row.placedEntryIdentity = source.placedEntryIdentity;
        row.positionBits = source.positionBits;
        row.objectIndex = source.objectIndex;
        row.pathOrdinal = source.pathOrdinal;
        row.declaredBubbleIndex = source.declaredBubbleIndex;
        row.actorDefinitionTag = source.actorDefinitionTag;
        row.sourceOffset = source.sourceOffset;
        row.quaternionBits = source.quaternionBits;
        row.uniformScaleBits = source.uniformScaleBits;
        row.nameHash = source.nameHash;
        row.placementFlagsRaw = source.placementFlagsRaw;
        row.auxiliaryRelative = source.auxiliaryRelative;
        row.complete = source.complete;
        output.placementContexts.push_back(std::move(row));
    }
    return true;
}

struct PlacementKey final {
    std::uint32_t scenarioIndex{};
    std::uint64_t identity{};

    bool operator==(const PlacementKey&) const = default;
};

struct PlacementKeyHash final {
    [[nodiscard]] std::size_t operator()(const PlacementKey& value) const noexcept {
        const std::uint64_t mixed = value.identity ^ (value.identity >> 32U)
                                    ^ (static_cast<std::uint64_t>(value.scenarioIndex) << 13U);
        return static_cast<std::size_t>(mixed);
    }
};

/** Materializes exact point/context cardinalities and every same-scenario identity match. */
[[nodiscard]] bool build_point_matches(GraphSnapshot& output) {
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> contextsByConfig{};
    for (std::uint32_t row = 0; row < output.configContexts.size(); ++row) {
        contextsByConfig[output.configContexts[row].configTag].push_back(row);
    }
    std::unordered_map<PlacementKey, std::vector<std::uint32_t>, PlacementKeyHash> placements{};
    for (std::uint32_t row = 0; row < output.placementContexts.size(); ++row) {
        const GraphPlacementContext& placement = output.placementContexts[row];
        placements[{placement.scenarioIndex, placement.placedEntryIdentity}].push_back(row);
    }
    for (std::uint32_t pointRow = 0; pointRow < output.points.size(); ++pointRow) {
        GraphPoint& point = output.points[pointRow];
        if (point.ruleRow >= output.rules.size()) {
            return false;
        }
        point.contexts.first = static_cast<std::uint32_t>(output.pointContexts.size());
        const GraphRule& rule = output.rules[point.ruleRow];
        const auto contexts = contextsByConfig.find(rule.configTag);
        if (contexts != contextsByConfig.end()) {
            for (const std::uint32_t configContextRow : contexts->second) {
                const GraphConfigContext& config = output.configContexts[configContextRow];
                GraphPointContext context{};
                context.globalRow = static_cast<std::uint32_t>(output.pointContexts.size());
                context.scenarioIndex = config.scenarioIndex;
                context.pointRow = pointRow;
                context.configContextRow = configContextRow;
                context.matches.first =
                    static_cast<std::uint32_t>(output.pointPlacementMatches.size());
                const auto found =
                    placements.find({config.scenarioIndex, point.placedEntryIdentity});
                if (found != placements.end()) {
                    context.matches.count = static_cast<std::uint32_t>(found->second.size());
                    for (const std::uint32_t placementContextRow : found->second) {
                        const GraphPlacementContext& placement =
                            output.placementContexts[placementContextRow];
                        output.pointPlacementMatches.push_back(
                            {static_cast<std::uint32_t>(output.pointPlacementMatches.size()),
                             config.scenarioIndex,
                             context.globalRow,
                             pointRow,
                             configContextRow,
                             placementContextRow,
                             point.placedEntryIdentity,
                             placement.occurrenceIndex == config.occurrenceIndex});
                    }
                }
                context.status = context.matches.count == 0   ? PointContextStatus::unresolved
                                 : context.matches.count == 1 ? PointContextStatus::exact
                                                              : PointContextStatus::ambiguous;
                output.pointContexts.push_back(context);
            }
        }
        point.contexts.count =
            static_cast<std::uint32_t>(output.pointContexts.size()) - point.contexts.first;
    }
    return true;
}

} // namespace

/** Builds one deterministic graph without publishing, serializing, or creating runnable squads. */
bool build_graph(const topology_inventory::Snapshot& topology,
                 const Facts& facts,
                 GraphSnapshot& output) noexcept {
    output = {};
    if (!topology.ready) {
        return false;
    }
    try {
        if (!detail::validate_facts(topology, facts)) {
            return false;
        }
        GraphSnapshot pending{};
        std::unordered_map<std::uint32_t, std::uint32_t> spawnersByConfig{};
        std::unordered_map<std::uint32_t, std::uint32_t> rulesByConfig{};
        if (!build_spawners(facts, pending, spawnersByConfig)
            || !build_rules(facts, pending, rulesByConfig)
            || !link_inline_rules(pending, spawnersByConfig) || !build_provenance(facts, pending)
            || !build_occurrence_contexts(topology, facts, spawnersByConfig, rulesByConfig, pending)
            || !build_point_matches(pending)
            || !detail::build_graph_edges(topology, facts, pending)) {
            return false;
        }
        pending.ready = true;
        output = std::move(pending);
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
