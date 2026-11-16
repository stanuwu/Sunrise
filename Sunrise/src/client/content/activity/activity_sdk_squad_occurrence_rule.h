#pragma once

#include <limits>
#include <map>
#include <span>
#include <tuple>
#include <vector>

#include "../../../middleware/content/packages/tables/authored_squad_reader.h"
#include "activity_sdk_spawn_reference.h"
#include "activity_sdk_squad_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::squad_inventory {

namespace tables = middleware::content::packages::tables;

/** The rule one spawner occurrence references, and the descriptor that places that rule. */
struct OccurrenceRule final {
    std::uint32_t ruleRow{format::kAbsentIndex};
    std::uint32_t descriptorRow{format::kAbsentIndex};

    [[nodiscard]] bool exact() const noexcept {
        return ruleRow != format::kAbsentIndex;
    }
};

/**
 * Joins a spawner to its spawn rule inside one object occurrence from the authored references
 * alone, for a rule whose points are placed outside the scenario's own object lists.
 */
class OccurrenceRuleIndex final {
public:
    OccurrenceRuleIndex(const topology_inventory::Snapshot& topology, const Facts& facts)
        : topology_(topology), facts_(facts), descriptorsByObject_(topology.objects.size()),
          contextsByOccurrence_(topology.occurrences.size()) {
        for (std::uint32_t row = 0; row < facts.descriptors.size(); ++row) {
            const DescriptorFact& descriptor = facts.descriptors[row];
            descriptorsByConfig_[descriptor.configTag].push_back(row);
            if (descriptor.objectIndex < descriptorsByObject_.size()) {
                descriptorsByObject_[descriptor.objectIndex].push_back(row);
            }
        }
        for (std::uint32_t row = 0; row < facts.configOccurrences.size(); ++row) {
            const std::uint32_t occurrence = facts.configOccurrences[row].occurrenceIndex;
            if (occurrence < contextsByOccurrence_.size()) {
                contextsByOccurrence_[occurrence].push_back(row);
            }
        }
        // A config tag two facts share names neither of them.
        for (std::uint32_t row = 0; row < facts.rules.size(); ++row) {
            const auto [entry, inserted] = rulesByConfig_.emplace(facts.rules[row].configTag, row);
            if (!inserted) {
                entry->second = format::kAbsentIndex;
            }
        }
        for (std::uint32_t row = 0; row < facts.spawners.size(); ++row) {
            const auto [entry, inserted] =
                spawnersByConfig_.emplace(facts.spawners[row].configTag, row);
            if (!inserted) {
                entry->second = format::kAbsentIndex;
            }
        }
        for (const topology_inventory::Occurrence& occurrence : topology.occurrences) {
            if (occurrence.objectIndex < topology.objects.size()) {
                ++objectKeyCounts_[{occurrence.scenarioIndex,
                                    occurrence.bubbleIndex,
                                    occurrence.stateIndex,
                                    topology.objects[occurrence.objectIndex].objectKey}];
            }
        }
    }

    /** @return The only spawner fact with this config tag, or kAbsentIndex. */
    [[nodiscard]] std::uint32_t spawner(std::uint32_t configTag) const noexcept {
        const auto found = spawnersByConfig_.find(configTag);
        return found == spawnersByConfig_.end() ? format::kAbsentIndex : found->second;
    }

    /** @return Every descriptor row that places this config tag. */
    [[nodiscard]] std::span<const std::uint32_t> sources(std::uint32_t configTag) const noexcept {
        const auto found = descriptorsByConfig_.find(configTag);
        return found == descriptorsByConfig_.end() ? std::span<const std::uint32_t>{}
                                                   : found->second;
    }

    /**
     * Resolves the spawner's rule reference inside one occurrence of its object.
     * @return An absent result unless the spawner slot, the referenced rule slot and the rule's
     * config are each the only match in that occurrence.
     */
    [[nodiscard]] OccurrenceRule resolve(std::uint32_t spawnerRow,
                                         std::uint32_t sourceRow,
                                         std::uint32_t occurrenceRow) const noexcept {
        if (!source_exact(spawnerRow, sourceRow, occurrenceRow)) {
            return {};
        }
        const SpawnerFact& authored = facts_.spawners[spawnerRow];
        const DescriptorFact& source = facts_.descriptors[sourceRow];
        const std::uint64_t reference = rule_reference(authored);
        if (reference == kNoReference
            || static_cast<std::uint32_t>(reference)
                   != topology_.objects[source.objectIndex].objectKey) {
            return {};
        }
        const auto ruleSlotIndex = static_cast<std::uint16_t>(reference >> 48U);
        OccurrenceRule result{};
        std::size_t targetCount = 0;
        for (const std::uint32_t row : descriptorsByObject_[source.objectIndex]) {
            const DescriptorFact& candidate = facts_.descriptors[row];
            if (candidate.slotIndex >= topology_.slots.size()) {
                return {};
            }
            const topology_inventory::Slot& slot = topology_.slots[candidate.slotIndex];
            if (slot.slotIndex != ruleSlotIndex
                || slot.slotType != tables::kAuthoredSquadRuleSlotType) {
                continue;
            }
            ++targetCount;
            const auto rule = rulesByConfig_.find(candidate.configTag);
            if (!candidate.complete || slot.objectIndex != source.objectIndex
                || candidate.componentClass != tables::kAuthoredSquadRulePrimaryClass
                || rule == rulesByConfig_.end() || rule->second == format::kAbsentIndex
                || !rule_exact(facts_.rules[rule->second], candidate, occurrenceRow)) {
                return {};
            }
            result = {rule->second, row};
        }
        return targetCount == 1 ? result : OccurrenceRule{};
    }

private:
    static constexpr std::uint64_t kNoReference = (std::numeric_limits<std::uint64_t>::max)();

    /**
     * @return The spawner's one rule reference, or kNoReference when either reference is
     * malformed, both are absent, they disagree, or the target is not a rule slot.
     */
    [[nodiscard]] static std::uint64_t rule_reference(const SpawnerFact& authored) noexcept {
        const SpawnReferenceState first = spawn_reference_state(authored.rawReference98);
        const SpawnReferenceState second = spawn_reference_state(authored.rawReferenceA0);
        if (first == SpawnReferenceState::invalid || second == SpawnReferenceState::invalid
            || (first == SpawnReferenceState::absent && second == SpawnReferenceState::absent)
            || (first == SpawnReferenceState::present && second == SpawnReferenceState::present
                && authored.rawReference98 != authored.rawReferenceA0)) {
            return kNoReference;
        }
        const std::uint64_t reference = first == SpawnReferenceState::present
                                            ? authored.rawReference98
                                            : authored.rawReferenceA0;
        return static_cast<std::uint8_t>(reference >> 32U) == tables::kAuthoredSquadRuleSlotType
                   ? reference
                   : kNoReference;
    }

    /** @return True when the spawner's own slot is the only one of its kind in the occurrence. */
    [[nodiscard]] bool source_exact(std::uint32_t spawnerRow,
                                    std::uint32_t sourceRow,
                                    std::uint32_t occurrenceRow) const noexcept {
        if (spawnerRow >= facts_.spawners.size() || sourceRow >= facts_.descriptors.size()
            || occurrenceRow >= topology_.occurrences.size()) {
            return false;
        }
        const SpawnerFact& authored = facts_.spawners[spawnerRow];
        const DescriptorFact& source = facts_.descriptors[sourceRow];
        const topology_inventory::Occurrence& occurrence = topology_.occurrences[occurrenceRow];
        if (!authored.complete || !source.complete || source.objectIndex != occurrence.objectIndex
            || source.objectIndex >= topology_.objects.size()
            || source.slotIndex >= topology_.slots.size()
            || occurrence.stateIndex >= topology_.states.size()
            || source.configTag != authored.configTag || spawner(authored.configTag) != spawnerRow
            || source.descriptorOffset != authored.secondaryComponentOffset
            || authored.primaryComponentClass != tables::kAuthoredSquadSpawnerPrimaryClass
            || authored.secondaryComponentClass != tables::kAuthoredSquadSpawnerSecondaryClass
            || source.componentClass != tables::kAuthoredSquadSpawnerPrimaryClass
            || source.senseSchema != format::kSquadSenseSchema
            || source.authSchema != format::kSquadAuthSchema) {
            return false;
        }
        const topology_inventory::Slot& slot = topology_.slots[source.slotIndex];
        const topology_inventory::State& state = topology_.states[occurrence.stateIndex];
        if (slot.objectIndex != source.objectIndex || slot.slotType != format::kSquadSlotType
            || slot.slotIndex >= (std::numeric_limits<std::uint16_t>::max)()
            || state.scenarioIndex != occurrence.scenarioIndex
            || state.bubbleIndex != occurrence.bubbleIndex
            || !unique_context(authored.configTag, occurrenceRow)) {
            return false;
        }
        // A slot reference names its object by key, so the key must name one object here.
        const auto keyCount =
            objectKeyCounts_.find({occurrence.scenarioIndex,
                                   occurrence.bubbleIndex,
                                   occurrence.stateIndex,
                                   topology_.objects[source.objectIndex].objectKey});
        if (keyCount == objectKeyCounts_.end() || keyCount->second != 1) {
            return false;
        }
        std::size_t slotDescriptors = 0;
        for (const std::uint32_t row : descriptorsByObject_[source.objectIndex]) {
            if (facts_.descriptors[row].slotIndex == source.slotIndex) {
                ++slotDescriptors;
            }
        }
        return slotDescriptors == 1;
    }

    /** @return True when a referenced rule is complete and each of its points is distinct. */
    [[nodiscard]] bool rule_exact(const RuleFact& rule,
                                  const DescriptorFact& descriptor,
                                  std::uint32_t occurrenceRow) const noexcept {
        if (!rule.complete || rule.inlineForm
            || descriptor.descriptorOffset != rule.secondaryComponentOffset
            || rule.primaryComponentClass != tables::kAuthoredSquadRulePrimaryClass
            || rule.secondaryComponentClass != tables::kAuthoredSquadRuleSecondaryClass
            || rule.points.empty() || !unique_context(rule.configTag, occurrenceRow)) {
            return false;
        }
        for (std::size_t point = 0; point < rule.points.size(); ++point) {
            const std::uint64_t identity = rule.points[point].placedEntryIdentity;
            if (identity == 0 || identity == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            for (std::size_t earlier = 0; earlier < point; ++earlier) {
                if (rule.points[earlier].placedEntryIdentity == identity) {
                    return false;
                }
            }
        }
        return true;
    }

    /** @return True when the occurrence holds this config once, in the bubble it declares. */
    [[nodiscard]] bool unique_context(std::uint32_t configTag,
                                      std::uint32_t occurrenceRow) const noexcept {
        const topology_inventory::Occurrence& occurrence = topology_.occurrences[occurrenceRow];
        std::size_t count = 0;
        for (const std::uint32_t row : contextsByOccurrence_[occurrenceRow]) {
            const ConfigOccurrenceFact& context = facts_.configOccurrences[row];
            if (context.configTag != configTag) {
                continue;
            }
            ++count;
            // A declared bubble of -1 places the config in every bubble.
            if (!context.complete || context.objectIndex != occurrence.objectIndex
                || (context.declaredBubbleIndex != -1
                    && (context.declaredBubbleIndex < 0
                        || static_cast<std::uint32_t>(context.declaredBubbleIndex)
                               != topology_.states[occurrence.stateIndex].entryIndex))) {
                return false;
            }
        }
        return count == 1;
    }

    const topology_inventory::Snapshot& topology_;
    const Facts& facts_;
    std::vector<std::vector<std::uint32_t>> descriptorsByObject_;
    std::vector<std::vector<std::uint32_t>> contextsByOccurrence_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> descriptorsByConfig_;
    std::map<std::uint32_t, std::uint32_t> rulesByConfig_;
    std::map<std::uint32_t, std::uint32_t> spawnersByConfig_;
    /** Occurrences per (scenario, bubble, state, object key). */
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::size_t>
        objectKeyCounts_;
};

} // namespace sunrise::client::content::activity::sdk_generation::squad_inventory
