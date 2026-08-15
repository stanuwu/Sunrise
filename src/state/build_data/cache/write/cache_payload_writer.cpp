#include "cache_payload_writer.h"

#include "../records/codec.h"

namespace sunrise::state::build_data::cache::writer {
namespace {

/**
 * Writes one whole object at the current file position.
 * @tparam Value Trivially copied cache object.
 * @param file Open temporary cache handle.
 * @param value Whole object to write.
 * @return True when Windows writes every byte.
 */
template <typename Value> [[nodiscard]] bool write_value(HANDLE file, const Value& value) noexcept {
    DWORD transferred = 0;
    return WriteFile(file, &value, static_cast<DWORD>(sizeof value), &transferred, nullptr) != FALSE
           && transferred == sizeof value;
}

/**
 * Encodes one domain and extends the shared checksum with each packed row.
 * @tparam Record Packed disk record type.
 * @tparam Value Runtime row type the codec overload picks.
 * @param values Complete sorted runtime rows.
 * @param checksum Running payload checksum.
 * @return True when every row has a valid disk form.
 */
template <typename Record, typename Value>
[[nodiscard]] bool checksum_domain(std::span<const Value> values,
                                   std::uint64_t& checksum) noexcept {
    for (const Value& value : values) {
        Record record{};
        if (!records::encode(value, record)) {
            return false;
        }
        checksum = records::checksum_value(checksum, record);
    }
    return true;
}

/**
 * Encodes and writes one whole domain in its checked sort order.
 * @tparam Record Packed disk record type.
 * @tparam Value Runtime row type the codec overload picks.
 * @param file Open temporary cache handle.
 * @param values Complete sorted runtime rows.
 * @return True when every packed row is written in full.
 */
template <typename Record, typename Value>
[[nodiscard]] bool write_domain(HANDLE file, std::span<const Value> values) noexcept {
    for (const Value& value : values) {
        Record record{};
        if (!records::encode(value, record) || !write_value(file, record)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Computes one checksum across every array in its fixed file order. */
bool payload_checksum(records::Domains domains, std::uint64_t& checksum) noexcept {
    checksum = records::checksum_value(records::kChecksumOffsetBasis, domains.constants);
    return checksum_domain<records::NamedRecord>(domains.named, checksum)
           && checksum_domain<records::ItemRecord>(domains.items, checksum)
           && checksum_domain<records::ItemDetailRecord>(domains.itemDetails, checksum)
           && checksum_domain<records::InventoryBucketRecord>(domains.inventoryBuckets, checksum)
           && checksum_domain<records::SocketEntryListRecord>(domains.socketEntryLists, checksum)
           && checksum_domain<records::SocketEntryTableRecord>(domains.socketEntryTables, checksum)
           && checksum_domain<records::AbilityBucketRecord>(domains.abilityBuckets, checksum)
           && checksum_domain<records::ProgressionRecord>(domains.progressions, checksum)
           && checksum_domain<records::ScenarioRecord>(domains.scenarios, checksum)
           && checksum_domain<records::RosterGroupRecord>(domains.rosterGroups, checksum)
           && checksum_domain<records::SpawnStemRecord>(domains.spawnStems, checksum)
           && checksum_domain<records::SpawnNameHashRecord>(domains.spawnNameHashes, checksum)
           && checksum_domain<records::HashNameRecord>(domains.hashNames, checksum);
}

/** Writes every array in the same order used by the payload checksum. */
bool write_payload(HANDLE file, records::Domains domains) noexcept {
    return write_domain<records::NamedRecord>(file, domains.named)
           && write_domain<records::ItemRecord>(file, domains.items)
           && write_domain<records::ItemDetailRecord>(file, domains.itemDetails)
           && write_domain<records::InventoryBucketRecord>(file, domains.inventoryBuckets)
           && write_domain<records::SocketEntryListRecord>(file, domains.socketEntryLists)
           && write_domain<records::SocketEntryTableRecord>(file, domains.socketEntryTables)
           && write_domain<records::AbilityBucketRecord>(file, domains.abilityBuckets)
           && write_domain<records::ProgressionRecord>(file, domains.progressions)
           && write_domain<records::ScenarioRecord>(file, domains.scenarios)
           && write_domain<records::RosterGroupRecord>(file, domains.rosterGroups)
           && write_domain<records::SpawnStemRecord>(file, domains.spawnStems)
           && write_domain<records::SpawnNameHashRecord>(file, domains.spawnNameHashes)
           && write_domain<records::HashNameRecord>(file, domains.hashNames);
}

} // namespace sunrise::state::build_data::cache::writer
