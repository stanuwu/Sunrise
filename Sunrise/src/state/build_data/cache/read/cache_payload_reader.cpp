#include "cache_payload_reader.h"

#include <algorithm>
#include <limits>

#include "../records/codec.h"
#include "../records/validation.h"

namespace sunrise::state::build_data::cache::read {
namespace {

/**
 * Adds one record array to the running cache size without unsigned overflow.
 * @param count Number of records.
 * @param stride Packed record size in bytes.
 * @param size Running file size.
 * @return True when the multiply and the add both fit.
 */
[[nodiscard]] bool
add_records(std::size_t count, std::size_t stride, std::uint64_t& size) noexcept {
    /** Cache offsets use the full unsigned 64-bit Windows file-size range. */
    constexpr std::uint64_t kMaximum = (std::numeric_limits<std::uint64_t>::max)();
    if (count > kMaximum / stride) {
        return false;
    }
    const std::uint64_t bytes = count * stride;
    if (bytes > kMaximum - size) {
        return false;
    }
    size += bytes;
    return true;
}

/**
 * Reads and decodes one record array, and extends the shared checksum.
 * @tparam Record Packed disk record type.
 * @tparam Value Runtime row type the codec overload picks.
 * @param file Open sequential cache handle.
 * @param output Span of rows to fill.
 * @param checksum Running payload checksum.
 * @return True when every packed row is complete and in its standard form.
 */
template <typename Record, typename Value>
[[nodiscard]] bool
read_domain(HANDLE file, std::span<Value> output, std::uint64_t& checksum) noexcept {
    for (Value& value : output) {
        Record record{};
        if (!read_value(file, record) || !records::decode(record, value)) {
            return false;
        }
        checksum = records::checksum_value(checksum, record);
    }
    return true;
}

} // namespace

/** Clears every output span so a failed read cannot expose partial records. */
void clear(records::MutableDomains output) noexcept {
    std::fill(output.positionProfiles.begin(),
              output.positionProfiles.end(),
              gameplay::entity_position_profiles::Row{});
    if (output.positionFingerprint != nullptr) {
        *output.positionFingerprint = {};
    }
    std::fill(
        output.objectTypes.begin(), output.objectTypes.end(), gameplay::entity_object_types::Row{});
    if (output.constants != nullptr) {
        *output.constants = {};
    }
    std::fill(output.named.begin(), output.named.end(), content::Definition{});
    std::fill(output.items.begin(), output.items.end(), items::Definition{});
    std::fill(output.collectibles.begin(), output.collectibles.end(), collectibles::Definition{});
    std::fill(output.materialRequirementSets.begin(),
              output.materialRequirementSets.end(),
              material_requirements::Definition{});
    std::fill(output.itemDetails.begin(), output.itemDetails.end(), items::details::Definition{});
    std::fill(
        output.socketPlugRules.begin(), output.socketPlugRules.end(), items::socket_plugs::Rule{});
    std::fill(
        output.socketPlugPools.begin(), output.socketPlugPools.end(), items::socket_plugs::Pool{});
    std::fill(output.socketPlugMembers.begin(),
              output.socketPlugMembers.end(),
              items::socket_plugs::Member{});
    std::fill(output.exoticCatalysts.begin(),
              output.exoticCatalysts.end(),
              items::catalysts::Definition{});
    std::fill(output.inventoryBuckets.begin(),
              output.inventoryBuckets.end(),
              inventory::buckets::Descriptor{});
    std::fill(output.socketEntryLists.begin(),
              output.socketEntryLists.end(),
              socket_entry_lists::Definition{});
    std::fill(output.socketEntryTables.begin(),
              output.socketEntryTables.end(),
              socket_entry_lists::EntryTable{});
    std::fill(output.abilityBuckets.begin(), output.abilityBuckets.end(), abilities::Definition{});
    std::fill(output.progressions.begin(), output.progressions.end(), progressions::Definition{});
    std::fill(output.records.begin(), output.records.end(), build_data::records::Definition{});
    std::fill(output.nodes.begin(), output.nodes.end(), nodes::Definition{});
    std::fill(output.sobjects.begin(), output.sobjects.end(), sobjects::Definition{});
    std::fill(output.scenarios.begin(), output.scenarios.end(), scenarios::Definition{});
    std::fill(output.rosterGroups.begin(), output.rosterGroups.end(), scenarios::RosterGroup{});
    std::fill(output.spawnStems.begin(), output.spawnStems.end(), spawn_sets::Stem{});
    std::fill(output.spawnNameHashes.begin(), output.spawnNameHashes.end(), spawn_sets::NameHash{});
    std::fill(output.spawnPoints.begin(), output.spawnPoints.end(), spawn_sets::Point{});
    std::fill(output.hashNames.begin(), output.hashNames.end(), hash_names::Name{});
    std::fill(output.vendorIndex.begin(), output.vendorIndex.end(), vendors::IndexEntry{});
    std::fill(
        output.vendorDefinitions.begin(), output.vendorDefinitions.end(), vendors::Definition{});
    std::fill(output.vendorSaleRows.begin(), output.vendorSaleRows.end(), vendors::SaleRow{});
    std::fill(output.vendorInstalledRows.begin(),
              output.vendorInstalledRows.end(),
              vendors::InstalledRow{});
    std::fill(output.recordObjectives.begin(),
              output.recordObjectives.end(),
              build_data::records::Objective{});
    std::fill(output.recordIntervals.begin(),
              output.recordIntervals.end(),
              build_data::records::Interval{});
    std::fill(
        output.recordRewards.begin(), output.recordRewards.end(), build_data::records::Reward{});
    std::fill(output.progressionSteps.begin(), output.progressionSteps.end(), progressions::Step{});
    std::fill(
        output.seasonPassRewards.begin(), output.seasonPassRewards.end(), season_pass::Reward{});
    std::fill(output.bounties.begin(), output.bounties.end(), bounties::Definition{});
    std::fill(output.rewardPools.begin(), output.rewardPools.end(), rewards::Pool{});
    std::fill(output.rewardEntries.begin(), output.rewardEntries.end(), rewards::Entry{});
    std::fill(output.rewardItems.begin(), output.rewardItems.end(), rewards::Item{});
    std::fill(
        output.rewardInstructions.begin(), output.rewardInstructions.end(), rewards::Instruction{});
    std::fill(output.rewardModifiers.begin(), output.rewardModifiers.end(), rewards::Modifier{});
    std::fill(output.rewardSockets.begin(), output.rewardSockets.end(), rewards::SocketOverride{});
}

/** Computes the exact file size for every record array. */
bool expected_size(const records::DomainCounts& counts, std::uint64_t& size) noexcept {
    size = sizeof(records::Header);
    return add_records(counts.named, sizeof(records::NamedRecord), size)
           && add_records(counts.items, sizeof(records::ItemRecord), size)
           && add_records(counts.collectibles, sizeof(records::CollectibleRecord), size)
           && add_records(
               counts.materialRequirementSets, sizeof(records::MaterialRequirementSetRecord), size)
           && add_records(counts.itemDetails, sizeof(records::ItemDetailRecord), size)
           && add_records(counts.socketPlugRules, sizeof(records::SocketPlugRuleRecord), size)
           && add_records(counts.socketPlugPools, sizeof(records::SocketPlugPoolRecord), size)
           && add_records(counts.socketPlugMembers, sizeof(records::SocketPlugMemberRecord), size)
           && add_records(counts.exoticCatalysts, sizeof(records::ExoticCatalystRecord), size)
           && add_records(counts.inventoryBuckets, sizeof(records::InventoryBucketRecord), size)
           && add_records(counts.socketEntryLists, sizeof(records::SocketEntryListRecord), size)
           && add_records(counts.socketEntryTables, sizeof(records::SocketEntryTableRecord), size)
           && add_records(counts.abilityBuckets, sizeof(records::AbilityBucketRecord), size)
           && add_records(counts.progressions, sizeof(records::ProgressionRecord), size)
           && add_records(counts.records, sizeof(records::RecordDefinitionRecord), size)
           && add_records(counts.nodes, sizeof(records::NodeDefinitionRecord), size)
           && add_records(counts.sobjects, sizeof(records::SObjectDefinitionRecord), size)
           && add_records(counts.scenarios, sizeof(records::ScenarioRecord), size)
           && add_records(counts.rosterGroups, sizeof(records::RosterGroupRecord), size)
           && add_records(counts.spawnStems, sizeof(records::SpawnStemRecord), size)
           && add_records(counts.spawnNameHashes, sizeof(records::SpawnNameHashRecord), size)
           && add_records(counts.spawnPoints, sizeof(records::SpawnPointRecord), size)
           && add_records(counts.hashNames, sizeof(records::HashNameRecord), size)
           && add_records(counts.vendorIndex, sizeof(records::VendorIndexRecord), size)
           && add_records(counts.vendorDefinitions, sizeof(records::VendorDefinitionRecord), size)
           && add_records(counts.vendorSaleRows, sizeof(records::VendorSaleRowRecord), size)
           && add_records(
               counts.vendorInstalledRows, sizeof(records::VendorInstalledRowRecord), size)
           && add_records(counts.positionProfiles, sizeof(records::PositionProfileRecord), size)
           && add_records(counts.objectTypes, sizeof(records::ObjectTypeRecord), size)
           && add_records(counts.recordObjectives, sizeof(records::RecordObjectiveRecord), size)
           && add_records(counts.recordIntervals, sizeof(records::RecordIntervalRecord), size)
           && add_records(counts.recordRewards, sizeof(records::RecordRewardRecord), size)
           && add_records(counts.progressionSteps, sizeof(records::ProgressionStepRecord), size)
           && add_records(counts.seasonPassRewards, sizeof(records::SeasonPassRewardRecord), size)
           && add_records(counts.bounties, sizeof(records::BountyRecord), size)
           && add_records(counts.rewardPools, sizeof(records::RewardPoolRecord), size)
           && add_records(counts.rewardEntries, sizeof(records::RewardEntryRecord), size)
           && add_records(counts.rewardItems, sizeof(records::RewardItemRecord), size)
           && add_records(counts.rewardInstructions, sizeof(records::RewardInstructionRecord), size)
           && add_records(counts.rewardModifiers, sizeof(records::RewardModifierRecord), size)
           && add_records(counts.rewardSockets, sizeof(records::RewardSocketOverrideRecord), size);
}

/** Reads every payload array and checks the decoded domains as one transaction. */
bool read_payload(HANDLE file,
                  const BuildIdentity& build,
                  const records::InvestmentConstants& constants,
                  const gameplay::entity_position_profiles::Fingerprint& fingerprint,
                  const records::DomainCounts& counts,
                  records::MutableDomains output,
                  std::uint64_t& checksum) noexcept {
    checksum = records::checksum_value(
        records::checksum_value(records::kChecksumOffsetBasis, constants), fingerprint);
    bool valid =
        read_domain<records::NamedRecord>(file, output.named.first(counts.named), checksum);
    valid =
        valid && read_domain<records::ItemRecord>(file, output.items.first(counts.items), checksum);
    valid = valid
            && read_domain<records::CollectibleRecord>(
                file, output.collectibles.first(counts.collectibles), checksum);
    valid =
        valid
        && read_domain<records::MaterialRequirementSetRecord>(
            file, output.materialRequirementSets.first(counts.materialRequirementSets), checksum);
    valid = valid
            && read_domain<records::ItemDetailRecord>(
                file, output.itemDetails.first(counts.itemDetails), checksum);
    valid = valid
            && read_domain<records::SocketPlugRuleRecord>(
                file, output.socketPlugRules.first(counts.socketPlugRules), checksum);
    valid = valid
            && read_domain<records::SocketPlugPoolRecord>(
                file, output.socketPlugPools.first(counts.socketPlugPools), checksum);
    valid = valid
            && read_domain<records::SocketPlugMemberRecord>(
                file, output.socketPlugMembers.first(counts.socketPlugMembers), checksum);
    valid = valid
            && read_domain<records::ExoticCatalystRecord>(
                file, output.exoticCatalysts.first(counts.exoticCatalysts), checksum);
    valid = valid
            && read_domain<records::InventoryBucketRecord>(
                file, output.inventoryBuckets.first(counts.inventoryBuckets), checksum);
    valid = valid
            && read_domain<records::SocketEntryListRecord>(
                file, output.socketEntryLists.first(counts.socketEntryLists), checksum);
    valid = valid
            && read_domain<records::SocketEntryTableRecord>(
                file, output.socketEntryTables.first(counts.socketEntryTables), checksum);
    valid = valid
            && read_domain<records::AbilityBucketRecord>(
                file, output.abilityBuckets.first(counts.abilityBuckets), checksum);
    valid = valid
            && read_domain<records::ProgressionRecord>(
                file, output.progressions.first(counts.progressions), checksum);
    valid = valid
            && read_domain<records::RecordDefinitionRecord>(
                file, output.records.first(counts.records), checksum);
    valid = valid
            && read_domain<records::NodeDefinitionRecord>(
                file, output.nodes.first(counts.nodes), checksum);
    valid = valid
            && read_domain<records::SObjectDefinitionRecord>(
                file, output.sobjects.first(counts.sobjects), checksum);
    valid = valid
            && read_domain<records::ScenarioRecord>(
                file, output.scenarios.first(counts.scenarios), checksum);
    valid = valid
            && read_domain<records::RosterGroupRecord>(
                file, output.rosterGroups.first(counts.rosterGroups), checksum);
    valid = valid
            && read_domain<records::SpawnStemRecord>(
                file, output.spawnStems.first(counts.spawnStems), checksum);
    valid = valid
            && read_domain<records::SpawnNameHashRecord>(
                file, output.spawnNameHashes.first(counts.spawnNameHashes), checksum);
    valid = valid
            && read_domain<records::SpawnPointRecord>(
                file, output.spawnPoints.first(counts.spawnPoints), checksum);
    valid = valid
            && read_domain<records::HashNameRecord>(
                file, output.hashNames.first(counts.hashNames), checksum);
    valid = valid
            && read_domain<records::VendorIndexRecord>(
                file, output.vendorIndex.first(counts.vendorIndex), checksum);
    valid = valid
            && read_domain<records::VendorDefinitionRecord>(
                file, output.vendorDefinitions.first(counts.vendorDefinitions), checksum);
    valid = valid
            && read_domain<records::VendorSaleRowRecord>(
                file, output.vendorSaleRows.first(counts.vendorSaleRows), checksum);
    valid = valid
            && read_domain<records::VendorInstalledRowRecord>(
                file, output.vendorInstalledRows.first(counts.vendorInstalledRows), checksum);
    valid = valid
            && read_domain<records::PositionProfileRecord>(
                file, output.positionProfiles.first(counts.positionProfiles), checksum);
    valid = valid
            && read_domain<records::ObjectTypeRecord>(
                file, output.objectTypes.first(counts.objectTypes), checksum);
    valid = valid
            && read_domain<records::RecordObjectiveRecord>(
                file, output.recordObjectives.first(counts.recordObjectives), checksum);
    valid = valid
            && read_domain<records::RecordIntervalRecord>(
                file, output.recordIntervals.first(counts.recordIntervals), checksum);
    valid = valid
            && read_domain<records::RecordRewardRecord>(
                file, output.recordRewards.first(counts.recordRewards), checksum);
    valid = valid
            && read_domain<records::ProgressionStepRecord>(
                file, output.progressionSteps.first(counts.progressionSteps), checksum);
    valid = valid
            && read_domain<records::SeasonPassRewardRecord>(
                file, output.seasonPassRewards.first(counts.seasonPassRewards), checksum);
    valid = valid
            && read_domain<records::BountyRecord>(
                file, output.bounties.first(counts.bounties), checksum);
    valid = valid
            && read_domain<records::RewardPoolRecord>(
                file, output.rewardPools.first(counts.rewardPools), checksum);
    valid = valid
            && read_domain<records::RewardEntryRecord>(
                file, output.rewardEntries.first(counts.rewardEntries), checksum);
    valid = valid
            && read_domain<records::RewardItemRecord>(
                file, output.rewardItems.first(counts.rewardItems), checksum);
    valid = valid
            && read_domain<records::RewardInstructionRecord>(
                file, output.rewardInstructions.first(counts.rewardInstructions), checksum);
    valid = valid
            && read_domain<records::RewardModifierRecord>(
                file, output.rewardModifiers.first(counts.rewardModifiers), checksum);
    valid = valid
            && read_domain<records::RewardSocketOverrideRecord>(
                file, output.rewardSockets.first(counts.rewardSockets), checksum);

    if (!valid) {
        return false;
    }
    return records::valid_domains(
        build,
        {
            constants,
            output.named.first(counts.named),
            output.items.first(counts.items),
            output.collectibles.first(counts.collectibles),
            output.materialRequirementSets.first(counts.materialRequirementSets),
            output.itemDetails.first(counts.itemDetails),
            output.socketPlugRules.first(counts.socketPlugRules),
            output.socketPlugPools.first(counts.socketPlugPools),
            output.socketPlugMembers.first(counts.socketPlugMembers),
            output.exoticCatalysts.first(counts.exoticCatalysts),
            output.inventoryBuckets.first(counts.inventoryBuckets),
            output.socketEntryLists.first(counts.socketEntryLists),
            output.socketEntryTables.first(counts.socketEntryTables),
            output.abilityBuckets.first(counts.abilityBuckets),
            output.progressions.first(counts.progressions),
            output.records.first(counts.records),
            output.nodes.first(counts.nodes),
            output.sobjects.first(counts.sobjects),
            output.scenarios.first(counts.scenarios),
            output.rosterGroups.first(counts.rosterGroups),
            output.spawnStems.first(counts.spawnStems),
            output.spawnNameHashes.first(counts.spawnNameHashes),
            output.spawnPoints.first(counts.spawnPoints),
            output.hashNames.first(counts.hashNames),
            output.vendorIndex.first(counts.vendorIndex),
            output.vendorDefinitions.first(counts.vendorDefinitions),
            output.vendorSaleRows.first(counts.vendorSaleRows),
            output.vendorInstalledRows.first(counts.vendorInstalledRows),
            output.positionProfiles.first(counts.positionProfiles),
            fingerprint,
            output.objectTypes.first(counts.objectTypes),
            output.recordObjectives.first(counts.recordObjectives),
            output.recordIntervals.first(counts.recordIntervals),
            output.recordRewards.first(counts.recordRewards),
            output.progressionSteps.first(counts.progressionSteps),
            output.seasonPassRewards.first(counts.seasonPassRewards),
            output.bounties.first(counts.bounties),
            output.rewardPools.first(counts.rewardPools),
            output.rewardEntries.first(counts.rewardEntries),
            output.rewardItems.first(counts.rewardItems),
            output.rewardInstructions.first(counts.rewardInstructions),
            output.rewardModifiers.first(counts.rewardModifiers),
            output.rewardSockets.first(counts.rewardSockets),
        });
}

} // namespace sunrise::state::build_data::cache::read
