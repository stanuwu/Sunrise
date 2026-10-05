/** Parses authored type-1 spawner and type-66 rule configs into normalized squad facts. */

#include <limits>
#include <utility>

#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "activity_sdk_squad_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail {
namespace {

namespace tables = middleware::content::packages::tables;

/** Reads one complete candidate row without trusting unbounded offset arithmetic. */
[[nodiscard]] bool candidate_placement(std::span<const std::byte> blob,
                                       const tables::AuthoredSquadMember& member,
                                       std::size_t variant,
                                       std::uint64_t index,
                                       CandidateFact& output) noexcept {
    output = {};
    tables::AuthoredSquadCandidate source{};
    if (!tables::authored_squad_candidate_record_at(blob, member, variant, index, source)) {
        return false;
    }
    output.candidateDescriptorOffset = source.descriptorOffset;
    output.placementRelative = source.placementRelative;
    output.placementOffset = source.placementOffset;
    output.candidateTail = source.descriptorTail;
    if (!source.hasPlacement) {
        return true;
    }
    output.placedEntryClass = source.placementClass;
    output.actorDefinitionTag = source.classDefinitionTag;
    output.quaternionBits = source.rotationBits;
    output.positionBits = source.positionBits;
    output.uniformScaleBits = source.uniformScaleBits;
    output.nameHash = source.nameHash;
    output.placementFlagsRaw = source.placementFlagsRaw;
    output.placedEntryIdentity = source.placementIdentity;
    output.state = CandidateState::exactPlacement;
    return true;
}

/** Compares two parsed candidate lanes exactly. */
[[nodiscard]] bool same_member(const MemberFact& left, const MemberFact& right) noexcept {
    if (left.memberKey != right.memberKey || left.reservedU32 != right.reservedU32) {
        return false;
    }
    for (std::size_t lane = 0; lane < left.candidates.size(); ++lane) {
        if (left.candidates[lane].size() != right.candidates[lane].size()) {
            return false;
        }
        for (std::size_t row = 0; row < left.candidates[lane].size(); ++row) {
            if (left.candidates[lane][row] != right.candidates[lane][row]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/** Compares repeated spawner definitions from different placed paths. */
[[nodiscard]] bool same_spawner(const SpawnerFact& left, const SpawnerFact& right) noexcept {
    if (left.configTag != right.configTag || left.rawReference98 != right.rawReference98
        || left.rawReferenceA0 != right.rawReferenceA0 || left.complete != right.complete
        || left.primaryComponentOffset != right.primaryComponentOffset
        || left.secondaryComponentOffset != right.secondaryComponentOffset
        || left.primaryComponentClass != right.primaryComponentClass
        || left.secondaryComponentClass != right.secondaryComponentClass
        || left.members.size() != right.members.size()
        || left.hasInlinePointSet != right.hasInlinePointSet
        || left.inlinePointSetInspected != right.inlinePointSetInspected
        || left.inlinePointSetOffset != right.inlinePointSetOffset
        || left.inlinePlacementComponentOffset != right.inlinePlacementComponentOffset
        || left.inlineInitialPointIndex != right.inlineInitialPointIndex
        || left.inlinePoints != right.inlinePoints
        || left.inlinePlacement != right.inlinePlacement) {
        return false;
    }
    for (std::size_t index = 0; index < left.members.size(); ++index) {
        if (!same_member(left.members[index], right.members[index])) {
            return false;
        }
    }
    return true;
}

/** Compares repeated spawn-rule definitions from different placed paths. */
[[nodiscard]] bool same_rule(const RuleFact& left, const RuleFact& right) noexcept {
    if (left.configTag != right.configTag || left.complete != right.complete
        || left.primaryComponentOffset != right.primaryComponentOffset
        || left.secondaryComponentOffset != right.secondaryComponentOffset
        || left.primaryComponentClass != right.primaryComponentClass
        || left.secondaryComponentClass != right.secondaryComponentClass
        || left.inlineForm != right.inlineForm || left.points.size() != right.points.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.points.size(); ++index) {
        if (left.points[index] != right.points[index]) {
            return false;
        }
    }
    return true;
}

/** Parses one exact authored type-1 config. */
[[nodiscard]] bool
parse_spawner(std::uint32_t tag, std::span<const std::byte> blob, SpawnerFact& output) {
    output = {};
    tables::AuthoredSquadSpawner source{};
    if (!tables::authored_squad_spawner(blob, tag, source)
        || source.members.count >= format::kAbsentIndex) {
        return false;
    }
    try {
        output.configTag = tag;
        output.rawReference98 = source.rawReference98;
        output.rawReferenceA0 = source.rawReferenceA0;
        output.primaryComponentOffset = source.components.primaryOffset;
        output.secondaryComponentOffset = source.components.secondaryOffset;
        output.primaryComponentClass = source.components.primaryClass;
        output.secondaryComponentClass = source.components.secondaryClass;
        output.members.reserve(static_cast<std::size_t>(source.members.count));
        for (std::uint64_t memberIndex = 0; memberIndex < source.members.count; ++memberIndex) {
            tables::AuthoredSquadMember member{};
            if (!tables::authored_squad_member_at(blob, source, memberIndex, member)) {
                return false;
            }
            MemberFact row{};
            row.memberKey = member.key;
            row.reservedU32 = member.reserved;
            for (std::size_t lane = 0; lane < member.candidates.size(); ++lane) {
                const tables::Array& candidates = member.candidates[lane];
                if (candidates.count > (std::numeric_limits<std::uint16_t>::max)()) {
                    return false;
                }
                row.candidates[lane].reserve(static_cast<std::size_t>(candidates.count));
                for (std::uint64_t candidateIndex = 0; candidateIndex < candidates.count;
                     ++candidateIndex) {
                    CandidateFact candidate{};
                    if (!candidate_placement(blob, member, lane, candidateIndex, candidate)) {
                        return false;
                    }
                    row.candidates[lane].push_back(candidate);
                }
            }
            output.members.push_back(std::move(row));
        }
        bool inlinePresent = false;
        tables::AuthoredSquadInlinePointSet inlineSet{};
        if (!tables::authored_squad_inline_point_set(blob, tag, source, inlinePresent, inlineSet)) {
            return false;
        }
        output.inlinePointSetInspected = true;
        if (inlinePresent) {
            if (inlineSet.points.count >= format::kAbsentIndex) {
                return false;
            }
            output.hasInlinePointSet = true;
            output.inlinePointSetOffset = inlineSet.pointSetOffset;
            output.inlinePlacementComponentOffset = inlineSet.placementComponentOffset;
            output.inlineInitialPointIndex = inlineSet.initialPointIndex;
            tables::AuthoredSquadRule pointOwner{};
            pointOwner.points = inlineSet.points;
            output.inlinePoints.reserve(static_cast<std::size_t>(inlineSet.points.count));
            for (std::uint64_t index = 0; index < inlineSet.points.count; ++index) {
                tables::AuthoredSquadRulePoint point{};
                if (!tables::authored_squad_rule_point_at(blob, pointOwner, index, point)) {
                    return false;
                }
                RulePointFact row{};
                row.placedEntryIdentity = point.placementIdentity;
                row.rowOffset = point.rowOffset;
                row.rawTail = point.rawTail;
                output.inlinePoints.push_back(row);
            }
            const tables::AuthoredSquadCandidate& placement = inlineSet.placement;
            CandidateFact& row = output.inlinePlacement;
            row.placementOffset = placement.placementOffset;
            row.placedEntryClass = placement.placementClass;
            row.actorDefinitionTag = placement.classDefinitionTag;
            row.quaternionBits = placement.rotationBits;
            row.positionBits = placement.positionBits;
            row.uniformScaleBits = placement.uniformScaleBits;
            row.nameHash = placement.nameHash;
            row.placementFlagsRaw = placement.placementFlagsRaw;
            row.placedEntryIdentity = placement.placementIdentity;
            row.state = CandidateState::exactPlacement;
        }
        output.complete = true;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}

/** Builds the rule row a spawner's own point set stands for. */
[[nodiscard]] RuleFact inline_rule(const SpawnerFact& spawner) {
    RuleFact rule{};
    rule.configTag = spawner.configTag;
    rule.points = spawner.inlinePoints;
    rule.complete = true;
    rule.primaryComponentOffset = spawner.inlinePointSetOffset;
    rule.secondaryComponentOffset = spawner.inlinePlacementComponentOffset;
    rule.primaryComponentClass = tables::kAuthoredSquadInlinePointSetClass;
    rule.secondaryComponentClass = tables::kAuthoredSquadInlinePlacementClass;
    rule.inlineForm = true;
    return rule;
}

/** Parses one exact authored type-66 config. */
[[nodiscard]] bool
parse_rule(std::uint32_t tag, std::span<const std::byte> blob, RuleFact& output) {
    output = {};
    tables::AuthoredSquadRule source{};
    if (!tables::authored_squad_rule(blob, tag, source)
        || source.points.count >= format::kAbsentIndex) {
        return false;
    }
    try {
        output.configTag = tag;
        output.primaryComponentOffset = source.components.primaryOffset;
        output.secondaryComponentOffset = source.components.secondaryOffset;
        output.primaryComponentClass = source.components.primaryClass;
        output.secondaryComponentClass = source.components.secondaryClass;
        output.points.reserve(static_cast<std::size_t>(source.points.count));
        for (std::uint64_t index = 0; index < source.points.count; ++index) {
            tables::AuthoredSquadRulePoint point{};
            if (!tables::authored_squad_rule_point_at(blob, source, index, point)) {
                return false;
            }
            RulePointFact row{};
            row.placedEntryIdentity = point.placementIdentity;
            row.rowOffset = point.rowOffset;
            row.rawTail = point.rawTail;
            output.points.push_back(row);
        }
        output.complete = true;
        return true;
    } catch (...) {
        output = {};
        return false;
    }
}
} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory::detail
