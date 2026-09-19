#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string_view>
#include <unordered_set>

#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "activity_sdk_squad_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail {
namespace {

namespace tables = middleware::content::packages::tables;
namespace topology = topology_inventory;

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

/** Derives one descriptor identity from its exact object and slot tuple. */
[[nodiscard]] bool descriptor_id(const topology::Snapshot& topology,
                                 const DescriptorFact& descriptor,
                                 std::string& output) {
    if (descriptor.objectIndex >= topology.objects.size()
        || descriptor.slotIndex >= topology.slots.size()) {
        return false;
    }
    const topology::Object& object = topology.objects[descriptor.objectIndex];
    const topology::Slot& slot = topology.slots[descriptor.slotIndex];
    if (slot.objectIndex != descriptor.objectIndex) {
        return false;
    }
    return format_text(output,
                       "descriptor/%08x/%08x/%08x/%04x/%04x",
                       static_cast<unsigned>(descriptor.configTag),
                       static_cast<unsigned>(object.objectTag),
                       static_cast<unsigned>(descriptor.descriptorOffset),
                       static_cast<unsigned>(slot.slotIndex),
                       static_cast<unsigned>(slot.slotType));
}

} // namespace

/** Checks the topology rows needed by the squad inventory. */
[[nodiscard]] bool valid_topology(const topology::Snapshot& source) noexcept {
    if (!source.ready || source.objects.size() >= format::kAbsentIndex
        || source.occurrences.size() >= format::kAbsentIndex
        || source.slots.size() >= format::kAbsentIndex
        || source.scenarios.size() >= format::kAbsentIndex) {
        return false;
    }
    for (std::size_t index = 0; index < source.objects.size(); ++index) {
        const topology::Object& object = source.objects[index];
        if (object.objectTag == 0 || object.objectTag == format::kAbsentIndex
            || object.firstSlot > source.slots.size()
            || object.slotCount > source.slots.size() - object.firstSlot) {
            return false;
        }
        for (std::size_t slot = object.firstSlot; slot < object.firstSlot + object.slotCount;
             ++slot) {
            if (source.slots[slot].objectIndex != index) {
                return false;
            }
        }
    }
    for (const topology::Occurrence& occurrence : source.occurrences) {
        std::string_view id{};
        if (occurrence.scenarioIndex >= source.scenarios.size()
            || occurrence.objectIndex >= source.objects.size()
            || occurrence.stateIndex >= source.states.size() || !text_view(occurrence.id, id)
            || id.empty()) {
            return false;
        }
        const topology::State& state = source.states[occurrence.stateIndex];
        if (state.scenarioIndex != occurrence.scenarioIndex) {
            return false;
        }
    }
    return true;
}

/** Validates all normalized facts before any output row is staged. */
[[nodiscard]] bool validate_facts(const topology::Snapshot& topology, const Facts& facts) {
    if (!facts.complete || facts.unresolvedReads != 0
        || facts.spawners.size() >= format::kAbsentIndex
        || facts.rules.size() >= format::kAbsentIndex
        || facts.descriptors.size() >= format::kAbsentIndex
        || facts.configOccurrences.size() >= format::kAbsentIndex
        || facts.placementOccurrences.size() >= format::kAbsentIndex) {
        return false;
    }
    std::unordered_set<std::uint32_t> spawnerTags{};
    for (const SpawnerFact& spawner : facts.spawners) {
        if (!spawner.complete || spawner.configTag == 0 || spawner.configTag == format::kAbsentIndex
            || !spawnerTags.insert(spawner.configTag).second
            || spawner.primaryComponentOffset < sizeof(std::uint32_t)
            || spawner.secondaryComponentOffset < sizeof(std::uint32_t)
            || spawner.primaryComponentOffset == spawner.secondaryComponentOffset
            || spawner.primaryComponentClass != tables::kAuthoredSquadSpawnerPrimaryClass
            || spawner.secondaryComponentClass != tables::kAuthoredSquadSpawnerSecondaryClass
            || spawner.members.size() >= format::kAbsentIndex
            || spawner.inlinePoints.size() >= format::kAbsentIndex
            || spawner.hasInlinePointSet != !spawner.inlinePoints.empty()) {
            return false;
        }
        if (spawner.hasInlinePointSet) {
            const CandidateFact& placement = spawner.inlinePlacement;
            if (spawner.inlinePointSetOffset < sizeof(std::uint32_t)
                || spawner.inlinePlacementComponentOffset < sizeof(std::uint32_t)
                || spawner.inlinePointSetOffset == spawner.inlinePlacementComponentOffset
                || placement.state != CandidateState::exactPlacement
                || placement.placementOffset == 0
                || placement.placedEntryClass != tables::kAuthoredSquadPlacementClass
                || !std::isfinite(std::bit_cast<float>(placement.uniformScaleBits))) {
                return false;
            }
            for (const std::uint32_t bits : placement.quaternionBits) {
                if (!std::isfinite(std::bit_cast<float>(bits))) {
                    return false;
                }
            }
            for (const std::uint32_t bits : placement.positionBits) {
                if (!std::isfinite(std::bit_cast<float>(bits))) {
                    return false;
                }
            }
            for (const RulePointFact& point : spawner.inlinePoints) {
                if (point.rowOffset == 0) {
                    return false;
                }
            }
        }
        for (const MemberFact& member : spawner.members) {
            for (const auto& lane : member.candidates) {
                if (lane.size() > (std::numeric_limits<std::uint16_t>::max)()) {
                    return false;
                }
                for (const CandidateFact& candidate : lane) {
                    if (candidate.candidateDescriptorOffset == 0) {
                        return false;
                    }
                    if (candidate.state == CandidateState::nullPlacement) {
                        if (candidate.placementRelative != 0 || candidate.placementOffset != 0
                            || candidate.placedEntryClass != format::kAbsentIndex
                            || candidate.actorDefinitionTag != format::kAbsentIndex) {
                            return false;
                        }
                        continue;
                    }
                    if (candidate.state != CandidateState::exactPlacement
                        || candidate.placementRelative == 0 || candidate.placementOffset == 0
                        || candidate.placedEntryClass != tables::kAuthoredSquadPlacementClass
                        || !std::isfinite(std::bit_cast<float>(candidate.uniformScaleBits))) {
                        return false;
                    }
                    for (const std::uint32_t bits : candidate.quaternionBits) {
                        if (!std::isfinite(std::bit_cast<float>(bits))) {
                            return false;
                        }
                    }
                    for (const std::uint32_t bits : candidate.positionBits) {
                        if (!std::isfinite(std::bit_cast<float>(bits))) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    std::unordered_set<std::uint32_t> ruleTags{};
    for (const RuleFact& rule : facts.rules) {
        const std::uint32_t primaryClass = rule.inlineForm
                                               ? tables::kAuthoredSquadInlinePointSetClass
                                               : tables::kAuthoredSquadRulePrimaryClass;
        const std::uint32_t secondaryClass = rule.inlineForm
                                                 ? tables::kAuthoredSquadInlinePlacementClass
                                                 : tables::kAuthoredSquadRuleSecondaryClass;
        if (!rule.complete || rule.configTag == 0 || rule.configTag == format::kAbsentIndex
            || !ruleTags.insert(rule.configTag).second
            || rule.primaryComponentOffset < sizeof(std::uint32_t)
            || rule.secondaryComponentOffset < sizeof(std::uint32_t)
            || rule.primaryComponentOffset == rule.secondaryComponentOffset
            || rule.primaryComponentClass != primaryClass
            || rule.secondaryComponentClass != secondaryClass
            || rule.points.size() >= format::kAbsentIndex
            || (rule.inlineForm && !spawnerTags.contains(rule.configTag))) {
            return false;
        }
        for (const RulePointFact& point : rule.points) {
            if (point.rowOffset == 0) {
                return false;
            }
        }
    }
    std::unordered_set<std::string> descriptorIds{};
    for (const DescriptorFact& descriptor : facts.descriptors) {
        std::string expected{};
        if (!descriptor.complete || descriptor.configTag == 0
            || descriptor.configTag == format::kAbsentIndex || descriptor.id.empty()
            || !descriptorIds.insert(descriptor.id).second
            || !descriptor_id(topology, descriptor, expected) || descriptor.id != expected) {
            return false;
        }
    }
    std::unordered_set<std::string> contextIds{};
    for (const ConfigOccurrenceFact& context : facts.configOccurrences) {
        if (!context.complete || context.configTag == 0 || context.configTag == format::kAbsentIndex
            || context.occurrenceIndex >= topology.occurrences.size() || context.id.empty()
            || context.objectIndex >= topology.objects.size()
            || topology.occurrences[context.occurrenceIndex].objectIndex != context.objectIndex
            || !contextIds.insert(context.id).second) {
            return false;
        }
        const topology::State& state =
            topology.states[topology.occurrences[context.occurrenceIndex].stateIndex];
        if (context.declaredBubbleIndex != tables::kGlobalBubbleIndex
            && (context.declaredBubbleIndex < 0
                || static_cast<std::uint32_t>(context.declaredBubbleIndex) != state.entryIndex)) {
            return false;
        }
    }
    contextIds.clear();
    for (const PlacementOccurrenceFact& placement : facts.placementOccurrences) {
        // A container or descriptor-embedded row belongs to no occurrence, so only its scenario,
        // its identity and its unique id can be checked here.
        if (placement.external) {
            if (!placement.complete || placement.scenarioIndex >= topology.scenarios.size()
                || placement.occurrenceIndex != format::kAbsentIndex
                || placement.placedEntryIdentity == 0
                || placement.placedEntryIdentity == (std::numeric_limits<std::uint64_t>::max)()
                || placement.id.empty() || !contextIds.insert(placement.id).second
                || !std::isfinite(std::bit_cast<float>(placement.uniformScaleBits))) {
                return false;
            }
            continue;
        }
        if (!placement.complete || placement.occurrenceIndex >= topology.occurrences.size()
            || placement.objectIndex >= topology.objects.size()
            || topology.occurrences[placement.occurrenceIndex].objectIndex != placement.objectIndex
            || placement.objectListTag == 0 || placement.objectListTag == format::kAbsentIndex
            || placement.sourceOffset == 0 || placement.id.empty()
            || !contextIds.insert(placement.id).second
            || !std::isfinite(std::bit_cast<float>(placement.uniformScaleBits))) {
            return false;
        }
        const topology::State& state =
            topology.states[topology.occurrences[placement.occurrenceIndex].stateIndex];
        if (placement.declaredBubbleIndex != tables::kGlobalBubbleIndex
            && (placement.declaredBubbleIndex < 0
                || static_cast<std::uint32_t>(placement.declaredBubbleIndex) != state.entryIndex)) {
            return false;
        }
        for (const std::uint32_t bits : placement.quaternionBits) {
            if (!std::isfinite(std::bit_cast<float>(bits))) {
                return false;
            }
        }
        for (const std::uint32_t bits : placement.positionBits) {
            if (!std::isfinite(std::bit_cast<float>(bits))) {
                return false;
            }
        }
    }
    std::unordered_set<std::uint32_t> schemaSlots{};
    for (const SlotSchemaFact& schema : facts.slotSchemas) {
        if (schema.slotIndex >= topology.slots.size()
            || !schemaSlots.insert(schema.slotIndex).second) {
            return false;
        }
    }
    if (!std::is_sorted(facts.actorDefinitionTags.begin(), facts.actorDefinitionTags.end())
        || std::adjacent_find(facts.actorDefinitionTags.begin(), facts.actorDefinitionTags.end())
               != facts.actorDefinitionTags.end()
        || std::any_of(facts.actorDefinitionTags.begin(),
                       facts.actorDefinitionTags.end(),
                       [](std::uint32_t tag) { return tag == 0 || tag == format::kAbsentIndex; })) {
        return false;
    }
    for (const SpawnerFact& spawner : facts.spawners) {
        for (const MemberFact& member : spawner.members) {
            for (const auto& lane : member.candidates) {
                for (const CandidateFact& candidate : lane) {
                    if (candidate.state == CandidateState::exactPlacement
                        && candidate.actorDefinitionTag != 0
                        && candidate.actorDefinitionTag != format::kAbsentIndex
                        && !std::binary_search(facts.actorDefinitionTags.begin(),
                                               facts.actorDefinitionTags.end(),
                                               candidate.actorDefinitionTag)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/** Builds one member row and leaves an exact actor index pending when needed. */
[[nodiscard]] bool build_member(const MemberFact& source,
                                std::string_view squadDigest,
                                std::uint32_t ordinal,
                                ActorResolver actorResolver,
                                void* actorContext,
                                SquadMember& output,
                                bool& actorLinksComplete) {
    output = {};
    if (!format_text(output.id,
                     "squad-member/%.*s/%06x",
                     static_cast<int>(squadDigest.size()),
                     squadDigest.data(),
                     static_cast<unsigned>(ordinal))) {
        return false;
    }
    output.memberOrdinal = ordinal;
    output.memberKey = source.memberKey;
    output.flags = format::kSquadMemberCandidateCountsComplete;
    std::size_t totalCandidates = 0;
    for (std::size_t lane = 0; lane < source.candidates.size(); ++lane) {
        if (source.candidates[lane].size() > (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        output.candidateCounts[lane] = static_cast<std::uint16_t>(source.candidates[lane].size());
        if (source.candidates[lane].size()
            > (std::numeric_limits<std::size_t>::max)() - totalCandidates) {
            return false;
        }
        totalCandidates += source.candidates[lane].size();
    }
    const bool invariant =
        std::all_of(output.candidateCounts.begin() + 1,
                    output.candidateCounts.end(),
                    [&output](std::uint16_t value) { return value == output.candidateCounts[0]; });
    if (invariant) {
        output.flags |= format::kSquadMemberCandidateCountsInvariant;
    }
    std::set<std::uint32_t> actorTags{};
    bool noNull = totalCandidates != 0;
    bool allActorsEligible = totalCandidates != 0;
    for (const auto& lane : source.candidates) {
        for (const CandidateFact& candidate : lane) {
            if (candidate.state != CandidateState::exactPlacement) {
                noNull = false;
                allActorsEligible = false;
                continue;
            }
            if (candidate.actorDefinitionTag == 0
                || candidate.actorDefinitionTag == format::kAbsentIndex) {
                allActorsEligible = false;
            } else {
                actorTags.insert(candidate.actorDefinitionTag);
            }
        }
    }
    if (noNull) {
        output.flags |= format::kSquadMemberNoNullCandidates;
    }
    bool commonProfileExact = allActorsEligible && actorResolver != nullptr;
    bool foundProfile = false;
    std::uint32_t singleActorIndex = format::kAbsentIndex;
    std::array<std::int8_t, 4> commonProfile{};
    if (commonProfileExact) {
        for (const std::uint32_t tag : actorTags) {
            std::uint32_t actorIndex = format::kAbsentIndex;
            std::array<std::int8_t, 4> profile{};
            if (!actorResolver(actorContext, tag, actorIndex, profile)
                || actorIndex == format::kAbsentIndex
                || !format::valid_authored_spawn_profile(profile)) {
                commonProfileExact = false;
                actorLinksComplete = false;
                continue;
            }
            if (actorTags.size() == 1) {
                singleActorIndex = actorIndex;
            }
            if (foundProfile && profile != commonProfile) {
                commonProfileExact = false;
            }
            commonProfile = profile;
            foundProfile = true;
        }
    }
    if (commonProfileExact && foundProfile) {
        output.flags |= format::kSquadMemberAuthoredProfileExact;
        output.authoredSpawnProfile = commonProfile;
    }
    if (allActorsEligible && actorTags.size() == 1) {
        output.actorDefinitionTag = *actorTags.begin();
        if (!format_text(output.actorDefinitionId,
                         "actor-class/%08x",
                         static_cast<unsigned>(output.actorDefinitionTag))) {
            return false;
        }
        if (singleActorIndex != format::kAbsentIndex) {
            output.actorClassIndex = singleActorIndex;
            output.actorLink = ActorLink::exactReciprocal;
            output.flags |= format::kSquadMemberActorClassExact;
        } else {
            output.actorLink = ActorLink::pendingDefinition;
            actorLinksComplete = false;
        }
    } else if (actorTags.size() > 1) {
        output.actorLink = ActorLink::inconsistentDefinitions;
    }
    const bool invariantReady = (output.flags & format::kSquadMemberInvariantReadyMask)
                                == format::kSquadMemberInvariantReadyMask;
    if (invariantReady && output.candidateCounts[0] != 0) {
        output.defaultCount = output.candidateCounts[0];
    }
    return true;
}

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail
