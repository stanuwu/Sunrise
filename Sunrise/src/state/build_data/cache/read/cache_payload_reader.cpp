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
               counts.vendorInstalledRows, sizeof(records::VendorInstalledRowRecord), size);
}

/** Reads every payload array and checks the decoded domains as one transaction. */
bool read_payload(HANDLE file,
                  const BuildIdentity& build,
                  const records::InvestmentConstants& constants,
                  const records::DomainCounts& counts,
                  records::MutableDomains output,
                  std::uint64_t& checksum) noexcept {
    checksum = records::checksum_value(records::kChecksumOffsetBasis, constants);
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
        });
}

} // namespace sunrise::state::build_data::cache::read
