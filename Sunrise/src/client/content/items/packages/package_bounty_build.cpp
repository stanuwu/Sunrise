#include <cstring>

#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::bounties;

/** @param blob Source bytes. @param offset Field offset. @param value Receives the field. */
template <typename Value>
[[nodiscard]] bool
read(std::span<const std::byte> blob, std::size_t offset, Value& value) noexcept {
    if (offset > blob.size() || blob.size() - offset < sizeof value) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

} // namespace

/**
 * Reads the item-type of every repeatable bounty, which is the key a vendor pool is grouped by.
 * No sale row and no vendor field names a pool, so the shared item-type pair is what joins one.
 * @param source Installed package source.
 * @param storage Pass storage holding the item rows and receiving the bounty rows.
 * @param itemCount Item rows this pass built.
 * @return True when the strings table read and produced at least one row.
 */
bool build_bounties(const reader::Source& source,
                    Storage& storage,
                    std::size_t itemCount) noexcept {
    storage.bountyCount = 0;
    std::uint32_t tableClass = 0;
    tables::Array rows{};
    if (!reader::read_tag(source,
                          storage.scratch,
                          tables::kItemStringsIndexTag,
                          storage.itemStringsTable,
                          tableClass)
        || !tables::find_array_at(std::span<const std::byte>{storage.itemStringsTable},
                                  offsetof(generated::C80805cdb, table),
                                  rows)
        || rows.elementClass != tables::kItemStringsIndexRowClass || rows.count < itemCount) {
        return false;
    }
    const std::span<const std::byte> table{storage.itemStringsTable};
    for (std::size_t item = 0; item < itemCount; ++item) {
        const state::build_data::items::Definition& definition = storage.rows[item];
        if (definition.bucketId != domain::kBountyBucketId
            || definition.tier != domain::kBountyTier) {
            continue;
        }
        if (storage.bountyCount >= domain::kDefinitionCapacity) {
            storage.bountyCount = 0;
            return false;
        }
        tables::IndexRow entry{};
        domain::Definition& row = storage.bountyRows[storage.bountyCount];
        row = {};
        row.itemIndex = definition.definitionIndex;
        std::uint16_t typeBank = 0;
        // A bounty whose strings blob will not read carries no pool key, so it is dropped rather
        // than published with a zero pair that would merge it into every other unnamed row.
        if (!tables::index_row(table, rows, definition.definitionIndex, entry)
            || !reader::read_tag(source, storage.scratch, entry.targetTag, storage.definition)
            || !read(std::span<const std::byte>{storage.definition},
                     tables::kItemStringsTypePairOffset,
                     typeBank)
            || !read(std::span<const std::byte>{storage.definition},
                     tables::kItemStringsTypeHashOffset,
                     row.itemType.hash)
            || row.itemType.hash == 0) {
            continue;
        }
        row.itemType.bank = typeBank;
        ++storage.bountyCount;
    }
    return storage.bountyCount != 0;
}

} // namespace sunrise::client::content::items::packages
