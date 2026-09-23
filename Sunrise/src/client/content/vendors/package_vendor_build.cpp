#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808078.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/build_data/vendors/definition.h"
#include "../../../state/runtime/vendor_reward_pool.h"
#include "layout.h"
#include "vendor_build.h"

namespace sunrise::client::content::vendors {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace domain = state::build_data::vendors;
namespace generated = middleware::content::packages::schema::generated;

/** Every extracted row, kept off the caller stack. */
struct Storage {
    std::vector<std::byte> blob{};
    std::array<domain::IndexEntry, domain::kIndexCapacity> index{};
    std::array<domain::Definition, domain::kDefinitionCapacity> definitions{};
    std::array<domain::SaleRow, domain::kSaleRowCapacity> saleRows{};
    std::array<domain::InstalledRow, domain::kInstalledRowCapacity> installedRows{};
    std::array<domain::TransferRule, domain::kTransferRuleCapacity> transferRules{};
    std::size_t indexCount{};
    std::size_t definitionCount{};
    std::size_t saleRowCount{};
    std::size_t installedRowCount{};
    std::size_t transferRuleCount{};
    /** Sale rows whose price depends on an expression, kept but never charged. */
    std::size_t conditionalRows{};
    /** Sale rows whose cost array did not read, kept but never charged. */
    std::size_t unreadableRows{};
    /** Sale rows with availability conditions. */
    std::size_t gatedRows{};
    /** Sale rows whose conditions did not read, bind or fit, kept but never sold. */
    std::size_t unreadableGates{};
    /** Definitions whose transfer array did not read, kept with no transfer rules. */
    std::size_t unreadableTransfers{};

    /** Only published prefixes are visible; extraction fully assigns each new row. */
    void reset() noexcept {
        blob.clear();
        indexCount = 0;
        definitionCount = 0;
        saleRowCount = 0;
        installedRowCount = 0;
        transferRuleCount = 0;
        conditionalRows = 0;
        unreadableRows = 0;
        gatedRows = 0;
        unreadableGates = 0;
        unreadableTransfers = 0;
    }
};

/** One array a definition or a sale row declares, reduced to what the catalog stores. */
struct ArrayView {
    std::uint32_t base{};
    std::uint32_t classId{};
    std::uint16_t count{};
};

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

/**
 * Reads one array descriptor and bounds it against the blob holding it.
 * The raw count is read first, because the shared resolver reports absent and corrupt alike.
 * @param blob Whole blob owning the descriptor.
 * @param descriptor Descriptor offset.
 * @param stride One row's size.
 * @param output Receives the array, or an absent array.
 * @return True when the array is absent, or resolves and ends inside the blob.
 */
[[nodiscard]] bool read_array(std::span<const std::byte> blob,
                              std::size_t descriptor,
                              std::size_t stride,
                              ArrayView& output) noexcept {
    /** Row counts are stored as unsigned 16-bit values. */
    constexpr std::uint64_t kMaximumCount = (std::numeric_limits<std::uint16_t>::max)();
    output = {};
    std::uint64_t declared = 0;
    if (!read(blob, descriptor, declared)) {
        return false;
    }
    if (declared == 0) {
        return true;
    }
    tables::Array array{};
    if (!tables::find_array_at(blob, descriptor, array) || array.count > kMaximumCount) {
        return false;
    }
    const std::uint64_t end = array.dataOffset + (array.count * stride);
    if (end > blob.size()) {
        return false;
    }
    output = {static_cast<std::uint32_t>(array.dataOffset),
              array.elementClass,
              static_cast<std::uint16_t>(array.count)};
    return true;
}

/**
 * Reads one sale row's cost entries and whether they are its price.
 * An entry is static only with both expression arrays empty and the plain trailing word. Any other
 * entry makes the row conditional: its entries are kept and the row is never charged.
 * @param blob Whole definition blob.
 * @param at Sale row offset inside the blob.
 * @param value Receives the entries and the row's price state.
 * @return True when the array is absent, or resolves, fits, and ends inside the blob. Otherwise
 *         the row is left unreadable, with no entries.
 */
[[nodiscard]] bool
read_sale_cost(std::span<const std::byte> blob, std::size_t at, domain::SaleRow& value) noexcept {
    value.costs = {};
    value.costCount = 0;
    value.priceState = domain::PriceState::unreadable;
    ArrayView cost{};
    if (!read_array(blob, at + kSaleCostArrayDescriptor, domain::kSaleCostRowStride, cost)) {
        return false;
    }
    if (cost.count != 0
        && (cost.classId != domain::kSaleCostRowClass || cost.count > value.costs.size())) {
        return false;
    }
    bool conditional = false;
    for (std::size_t entry = 0; entry < cost.count; ++entry) {
        const std::size_t entryAt = cost.base + (entry * domain::kSaleCostRowStride);
        domain::SaleCost& output = value.costs[entry];
        std::uint64_t firstProgram = 0;
        std::uint64_t secondProgram = 0;
        std::uint32_t word = 0;
        if (!read(blob, entryAt + kSaleCostItemIndexOffset, output.itemIndex)
            || !read(blob, entryAt + kSaleCostQuantityOffset, output.quantity)
            || !read(blob, entryAt + kSaleCostFirstProgramDescriptor, firstProgram)
            || !read(blob, entryAt + kSaleCostSecondProgramDescriptor, secondProgram)
            || !read(blob, entryAt + kSaleCostWordOffset, word)) {
            value.costs = {};
            return false;
        }
        conditional = conditional || firstProgram != 0 || secondProgram != 0
                      || word != domain::kPlainCostWord;
    }
    value.costCount = static_cast<std::uint8_t>(cost.count);
    value.priceState = conditional ? domain::PriceState::conditional : domain::PriceState::plain;
    return true;
}

/**
 * Binds one sale row's availability conditions. A list that does not read, bind or fit is kept as
 * an unreadable row, which refuses the purchase.
 * @param conditions Loaded condition tables the expressions bind through.
 * @param blob Whole definition blob.
 * @param at Sale row offset inside the blob.
 * @param value Receives the bound conditions.
 * @param storage Pass storage counting gated and unreadable rows.
 * @return False only when the reader's storage failed, which a later pass retries.
 */
[[nodiscard]] bool read_availability(const items::packages::RewardConditions& conditions,
                                     std::span<const std::byte> blob,
                                     std::size_t at,
                                     domain::SaleRow& value,
                                     Storage& storage) noexcept {
    std::size_t count = 0;
    items::packages::BankFailure failure = items::packages::BankFailure::none;
    const bool readable = conditions.read_list(blob,
                                               at + kSaleAvailabilityDescriptor,
                                               value.availability,
                                               count,
                                               failure,
                                               kSaleAvailabilityRowClass);
    if (failure == items::packages::BankFailure::storage) {
        return false;
    }
    value.availabilityUnreadable = !readable;
    value.availabilityCount = readable ? static_cast<std::uint8_t>(count) : 0;
    if (!readable) {
        value.availability = {};
        ++storage.unreadableGates;
    } else if (count != 0) {
        ++storage.gatedRows;
    }
    return true;
}

/**
 * Reads the whole installed vendor index.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Pass storage receiving the index rows.
 * @return True when the index blob reads and every row fits.
 */
[[nodiscard]] bool
read_index(const reader::Source& source, reader::Scratch& scratch, Storage& storage) noexcept {
    std::uint32_t classId = 0;
    tables::Array array{};
    if (!reader::read_tag(source, scratch, kIndexRootTag, storage.blob, classId)
        || classId != domain::kIndexWrapperClass) {
        return false;
    }
    const std::span<const std::byte> blob{storage.blob};
    if (!tables::find_array_at(blob, offsetof(generated::SVendorIndex, table), array)
        || array.elementClass != domain::kIndexRowClass || array.count > domain::kIndexCapacity) {
        return false;
    }
    for (std::uint64_t row = 0; row < array.count; ++row) {
        tables::IndexRow entry{};
        if (!tables::index_row(blob, array, row, entry)) {
            return false;
        }
        storage.index[storage.indexCount] = {
            entry.definitionHash, entry.targetTag, static_cast<std::uint16_t>(row)};
        ++storage.indexCount;
    }
    return storage.indexCount != 0;
}

/**
 * Reads every sale row of one definition into the flat bank.
 * A row whose cost array does not read costs that row its price, never the vendor: a purchase
 * names the row by ordinal, so the row stays in place and refuses to be bought.
 * @param conditions Loaded condition tables the availability expressions bind through.
 * @param blob Whole definition blob.
 * @param definition Definition whose sale array was already resolved.
 * @param storage Pass storage receiving the rows.
 * @return True when every row is inside the blob and the bank holds them all.
 */
[[nodiscard]] bool read_sale_rows(const items::packages::RewardConditions& conditions,
                                  std::span<const std::byte> blob,
                                  const domain::Definition& definition,
                                  Storage& storage) noexcept {
    if (definition.saleCount > domain::kSaleRowCapacity - storage.saleRowCount) {
        return false;
    }
    for (std::size_t row = 0; row < definition.saleCount; ++row) {
        const std::size_t at = definition.saleRowBase + (row * domain::kSaleRowStride);
        domain::SaleRow& value = storage.saleRows[storage.saleRowCount + row];
        value = {};
        if (!read(blob, at + kSaleItemIndexOffset, value.itemIndex)
            || !read(blob, at + kSaleSecondaryItemOffset, value.secondaryItemIndex)
            || !read(blob, at + kSaleCategoryIndexOffset, value.categoryIndex)) {
            return false;
        }
        if (!read_sale_cost(blob, at, value)) {
            ++storage.unreadableRows;
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=build_data stage=vendors result=unreadable_cost hash=0x%08X "
                              "row=%zu",
                              definition.definitionHash,
                              row);
        } else if (value.priceState == domain::PriceState::conditional) {
            ++storage.conditionalRows;
        }
        if (!read_availability(conditions, blob, at, value, storage)) {
            return false;
        }
    }
    storage.saleRowCount += definition.saleCount;
    return true;
}

/**
 * Reads the definition hash of every category row of one definition into the flat bank.
 * @param blob Whole definition blob.
 * @param definition Definition whose installed array was already resolved.
 * @param storage Pass storage receiving the rows.
 * @return True when every row is inside the blob and the bank holds them all.
 */
[[nodiscard]] bool read_installed_rows(std::span<const std::byte> blob,
                                       const domain::Definition& definition,
                                       Storage& storage) noexcept {
    if (definition.installedCount > domain::kInstalledRowCapacity - storage.installedRowCount) {
        return false;
    }
    for (std::size_t row = 0; row < definition.installedCount; ++row) {
        const std::size_t at = definition.installedRowBase + (row * domain::kInstalledRowStride);
        domain::InstalledRow& value = storage.installedRows[storage.installedRowCount + row];
        value = {};
        if (!read(blob, at + kInstalledRowHashOffset, value.definitionHash)) {
            return false;
        }
    }
    storage.installedRowCount += definition.installedCount;
    return true;
}

// The generated rule class is two unnamed bytes, read in the manifest's accepted-item order:
// the accepted source bucket, then its destination.
static_assert(sizeof(domain::TransferRule) == generated::C8080785f::kSize);

/**
 * Reads every transfer rule of one definition into the flat bank.
 * @param blob Whole definition blob.
 * @param definition Definition receiving its rule count; its bank offset is already set.
 * @param storage Pass storage receiving the rules.
 * @return True when the array is absent, or has the rule class, fits the bank, and every rule is
 *         inside the blob. Otherwise the definition keeps no rules and the bank is left as found.
 */
[[nodiscard]] bool read_transfer_rules(std::span<const std::byte> blob,
                                       domain::Definition& definition,
                                       Storage& storage) noexcept {
    definition.transferRuleCount = 0;
    ArrayView transfers{};
    if (!read_array(blob, kTransferArrayDescriptor, sizeof(domain::TransferRule), transfers)
        || (transfers.count != 0 && transfers.classId != generated::C8080785f::kClass)
        || transfers.count > domain::kTransferRuleCapacity - storage.transferRuleCount) {
        return false;
    }
    for (std::size_t row = 0; row < transfers.count; ++row) {
        const std::size_t at = transfers.base + (row * sizeof(domain::TransferRule));
        if (!read(blob, at, storage.transferRules[storage.transferRuleCount + row])) {
            return false;
        }
    }
    definition.transferRuleCount = transfers.count;
    storage.transferRuleCount += transfers.count;
    return true;
}

/**
 * Reads one vendor definition and every row array it owns.
 * @param conditions Loaded condition tables the availability expressions bind through.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param entry Index row naming the definition.
 * @param storage Pass storage receiving the definition and its rows.
 * @return True when the definition blob reads and every array ends inside it.
 */
[[nodiscard]] bool read_definition(const items::packages::RewardConditions& conditions,
                                   const reader::Source& source,
                                   reader::Scratch& scratch,
                                   const domain::IndexEntry& entry,
                                   Storage& storage) noexcept {
    /** Definition sizes are stored as unsigned 32-bit values. */
    constexpr std::size_t kMaximumSize = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t classId = 0;
    if (storage.definitionCount == domain::kDefinitionCapacity
        || !reader::read_tag(source, scratch, entry.definitionTag, storage.blob, classId)
        || classId != domain::kDefinitionClass || storage.blob.size() > kMaximumSize) {
        return false;
    }
    const std::span<const std::byte> blob{storage.blob};
    ArrayView installed{};
    ArrayView sale{};
    ArrayView interactions{};
    if (!read_array(blob, kInstalledArrayDescriptor, domain::kInstalledRowStride, installed)
        || !read_array(blob, kSaleArrayDescriptor, domain::kSaleRowStride, sale)
        || !read_array(
            blob, kInteractionArrayDescriptor, domain::kInteractionRowStride, interactions)) {
        return false;
    }
    domain::Definition definition{};
    definition.definitionHash = entry.definitionHash;
    definition.definitionTag = entry.definitionTag;
    definition.definitionClass = classId;
    definition.definitionSize = static_cast<std::uint32_t>(blob.size());
    definition.index = entry.index;
    definition.installedRowBase = installed.base;
    definition.installedRowClass = installed.classId;
    definition.installedCount = installed.count;
    definition.saleRowBase = sale.base;
    definition.saleRowClass = sale.classId;
    definition.saleCount = sale.count;
    definition.interactionRowBase = interactions.base;
    definition.interactionRowClass = interactions.classId;
    definition.interactionCount = interactions.count;
    definition.saleRowOffset = static_cast<std::uint32_t>(storage.saleRowCount);
    definition.installedRowOffset = static_cast<std::uint32_t>(storage.installedRowCount);
    definition.transferRuleOffset = static_cast<std::uint32_t>(storage.transferRuleCount);
    // A skipped definition must leave the banks exactly as it found them; an orphan sale row
    // shifts the next definition's offset and `valid()` then rejects the whole set.
    const std::size_t saleRowsBefore = storage.saleRowCount;
    const std::size_t installedRowsBefore = storage.installedRowCount;
    const std::size_t transferRulesBefore = storage.transferRuleCount;
    // An unreadable transfer array costs this vendor its transfers alone, never its sales.
    if (!read_transfer_rules(blob, definition, storage)) {
        ++storage.unreadableTransfers;
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=build_data stage=vendors result=unreadable_transfers hash=0x%08X",
                          definition.definitionHash);
    }
    if (!read(blob, kResetIntervalOffset, definition.resetIntervalRaw)
        || !read(blob, kResetPhaseOffset, definition.resetPhaseRaw)
        || !read_sale_rows(conditions, blob, definition, storage)
        || !read_installed_rows(blob, definition, storage)) {
        storage.saleRowCount = saleRowsBefore;
        storage.installedRowCount = installedRowsBefore;
        storage.transferRuleCount = transferRulesBefore;
        return false;
    }
    storage.definitions[storage.definitionCount] = definition;
    ++storage.definitionCount;
    return true;
}

/**
 * Reports the pass so a boot with no vendor catalog says which step lost the rows.
 * @param storage Pass storage holding every count.
 * @param skipped Requested definitions that could not be read or could not fit.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage, std::size_t skipped, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=vendors index=%zu definitions=%zu "
                                      "sale=%zu conditional=%zu unreadable=%zu gated=%zu "
                                      "unreadable_gates=%zu installed=%zu transfers=%zu "
                                      "unreadable_transfers=%zu skipped=%zu result=%s",
                                      storage.indexCount,
                                      storage.definitionCount,
                                      storage.saleRowCount,
                                      storage.conditionalRows,
                                      storage.unreadableRows,
                                      storage.gatedRows,
                                      storage.unreadableGates,
                                      storage.installedRowCount,
                                      storage.transferRuleCount,
                                      storage.unreadableTransfers,
                                      skipped,
                                      result);
    if (written > 0) {
        const bool clean = storage.indexCount != 0 && skipped == 0 && storage.unreadableRows == 0
                           && storage.unreadableTransfers == 0;
        core::log::write(core::log::Channel::state,
                         clean ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Only installed instanced gear may enter the supported one-item payout pool.
 * @param itemIndex Native item-definition index in one reward entry.
 * @param hash Receives the item hash on success.
 * @return False for missing rows, quests, cosmetics or non-equippable items.
 */
[[nodiscard]] bool reward_gear(std::uint16_t itemIndex, std::uint32_t& hash) noexcept {
    state::build_data::items::Definition item{};
    state::build_data::items::details::Definition detail{};
    if (!state::build_data::find_item_definition_index(itemIndex, item)
        || item.questInitialization.scope
               != state::build_data::items::QuestInitialization::Scope::none
        || !state::build_data::find_configured_item_detail(itemIndex, detail)
        || detail.definitionIndex != itemIndex || detail.definitionHash != item.definitionHash
        || detail.bucketId != item.bucketId || !detail.equipmentSlot.has_value()
        || detail.instancedDefinitionState
               != state::build_data::items::details::InstancedDefinitionState::instanced) {
        return false;
    }
    hash = item.definitionHash;
    return true;
}

/**
 * Gear leaves accept no gate or one class FLAG instruction; other programs are not payout data.
 * @param blob Whole reward-list table.
 * @param entry Offset of the entry holding the expression.
 * @param mask Receives permitted character-class bits on success.
 * @return False when the expression uses an unsupported operation or flag.
 */
[[nodiscard]] bool
reward_class_mask(std::span<const std::byte> blob, std::size_t entry, std::uint8_t& mask) noexcept {
    ArrayView expression{};
    if (!read_array(blob,
                    entry + kRewardEntryConditionOffset,
                    tables::kUnlockInstructionStride,
                    expression)) {
        return false;
    }
    if (expression.count == 0) {
        mask = kAllCharacterClasses;
        return true;
    }
    std::uint32_t opcode = 0, operand = 0;
    if (expression.count != 1 || expression.classId != tables::kInvestmentExpressionRowClass
        || !read(blob, expression.base, opcode)
        || !read(blob, expression.base + tables::kUnlockInstructionOperandOffset, operand)
        || opcode != tables::kUnlockReadFlagOpcode) {
        return false;
    }
    switch (operand) {
    case kTitanArmourFlag:
        mask = 1U << static_cast<std::uint8_t>(state::CharacterClass::titan);
        return true;
    case kHunterArmourFlag:
        mask = 1U << static_cast<std::uint8_t>(state::CharacterClass::hunter);
        return true;
    case kWarlockArmourFlag:
        mask = 1U << static_cast<std::uint8_t>(state::CharacterClass::warlock);
        return true;
    default:
        return false;
    }
}

/**
 * Appends one gear hash only to the classes admitted by its installed expression.
 * @param pool Candidate package pool under construction.
 * @param hash Installed gear item hash.
 * @param classMask Allowed character classes.
 * @return False when a class list exceeds the supported capacity.
 */
[[nodiscard]] bool
append_reward(rewards::Pool& pool, std::uint32_t hash, std::uint8_t classMask) noexcept {
    for (std::size_t classIndex = 0; classIndex < pool.items.size(); ++classIndex) {
        if ((classMask & (1U << classIndex)) == 0) {
            continue;
        }
        auto& count = pool.counts[classIndex];
        if (count == rewards::kCandidateCapacity) {
            return false;
        }
        pool.items[classIndex][count++] = hash;
    }
    return true;
}

/**
 * Resolves one reward-list row without treating a selector as a direct item index.
 * @param blob Whole reward-list table.
 * @param lists Bounded top-level list array.
 * @param listIndex Native reward-list selector.
 * @param entries Receives the selected entry array on success.
 * @return False for an absent, malformed or wrong-class list.
 */
[[nodiscard]] bool reward_entries(std::span<const std::byte> blob,
                                  const tables::Array& lists,
                                  std::uint16_t listIndex,
                                  ArrayView& entries) noexcept {
    if (listIndex >= lists.count) {
        return false;
    }
    const auto row =
        lists.dataOffset + (static_cast<std::size_t>(listIndex) * kRewardListRowStride);
    return read_array(blob, row + tables::kTableArrayDescriptor, kRewardEntryStride, entries)
           && entries.classId == kRewardEntryClass;
}

/**
 * Reads one direct child list; nested and non-gear leaves cannot become one-gear payouts.
 * @param blob Whole reward-list table.
 * @param lists Bounded top-level list array.
 * @param child Native selector of the direct child list.
 * @param pool Receives supported gear candidates.
 * @return False when a gear leaf has an unknown condition or malformed layout.
 */
[[nodiscard]] bool read_reward_branch(std::span<const std::byte> blob,
                                      const tables::Array& lists,
                                      std::uint16_t child,
                                      rewards::Pool& pool) noexcept {
    ArrayView entries{};
    if (!reward_entries(blob, lists, child, entries)) {
        return false;
    }
    for (std::size_t index = 0; index < entries.count; ++index) {
        const auto at = entries.base + (index * kRewardEntryStride);
        std::uint16_t itemIndex = kNoRewardSelector, nested = 0, other = 0;
        if (!read(blob, at + kRewardEntryItemOffset, itemIndex)
            || !read(blob, at + kRewardEntryChildOffset, nested)
            || !read(blob, at + kRewardEntryOtherSelectorOffset, other)) {
            return false;
        }
        std::uint32_t hash = 0;
        if (itemIndex == kNoRewardSelector || !reward_gear(itemIndex, hash)) {
            continue;
        }
        std::uint8_t classMask = 0;
        if (nested != kNoRewardSelector || other != kNoRewardSelector
            || !reward_class_mask(blob, at, classMask) || !append_reward(pool, hash, classMask)) {
            return false;
        }
    }
    return true;
}

/**
 * Reads a package's sack selector from its installed item definition.
 * @param source Installed package directory and borrowed keys.
 * @param scratch Shared package block storage.
 * @param itemTable Installed item index table bytes.
 * @param itemRows Item index array within itemTable.
 * @param packageHash Package item hash to resolve.
 * @param definition Reusable storage for the package definition.
 * @param listIndex Receives the native reward-list selector.
 * @return False when the package or its sack block is unreadable.
 */
[[nodiscard]] bool package_reward_list(const reader::Source& source,
                                       reader::Scratch& scratch,
                                       std::span<const std::byte> itemTable,
                                       const tables::Array& itemRows,
                                       std::uint32_t packageHash,
                                       std::vector<std::byte>& definition,
                                       std::uint16_t& listIndex) noexcept {
    state::build_data::items::Definition package{};
    tables::IndexRow index{};
    std::uint32_t classId = 0, marker = 0;
    if (!state::build_data::find_item_definition_hash(packageHash, package)
        || package.definitionIndex >= itemRows.count
        || !tables::index_row(itemTable, itemRows, package.definitionIndex, index)
        || index.definitionHash != packageHash
        || !reader::read_tag(source, scratch, index.targetTag, definition, classId)
        || classId != tables::kItemDefinitionClass) {
        return false;
    }
    const std::span<const std::byte> blob{definition};
    std::int64_t relative = 0;
    if (!read(blob, kItemRewardSackPointer, relative)
        || relative < -static_cast<std::int64_t>(kItemRewardSackPointer)
        || relative > static_cast<std::int64_t>(blob.size() - kItemRewardSackPointer)) {
        return false;
    }
    const auto block =
        static_cast<std::size_t>(static_cast<std::int64_t>(kItemRewardSackPointer) + relative);
    ArrayView sackEntries{};
    return block >= sizeof marker && read(blob, block - sizeof marker, marker)
           && marker == kRewardSackClass && read(blob, block + kRewardSackListOffset, listIndex)
           && read_array(
               blob, block + kRewardSackEntriesOffset, kRewardSackEntryStride, sackEntries)
           && sackEntries.classId == kRewardSackEntryClass;
}

/**
 * Reads direct gear branches only; the other selector's target is unresolved.
 * @param source Installed package directory and borrowed keys.
 * @param scratch Shared package block storage.
 * @param itemTable Installed item index table bytes.
 * @param itemRows Item index array within itemTable.
 * @param listsBlob Whole reward-list table.
 * @param lists Bounded top-level reward-list array.
 * @param packageHash Package item hash to resolve.
 * @param pool Receives supported class-specific gear.
 * @param definition Reusable package-definition storage.
 * @return False when the supported tree has an unreadable list or gear gate.
 */
[[nodiscard]] bool read_reward_pool(const reader::Source& source,
                                    reader::Scratch& scratch,
                                    std::span<const std::byte> itemTable,
                                    const tables::Array& itemRows,
                                    std::span<const std::byte> listsBlob,
                                    const tables::Array& lists,
                                    std::uint32_t packageHash,
                                    rewards::Pool& pool,
                                    std::vector<std::byte>& definition) noexcept {
    pool = {};
    pool.packageHash = packageHash;
    std::uint16_t parent = kNoRewardSelector;
    ArrayView entries{};
    if (!package_reward_list(source, scratch, itemTable, itemRows, packageHash, definition, parent)
        || !reward_entries(listsBlob, lists, parent, entries)) {
        return false;
    }
    for (std::size_t index = 0; index < entries.count; ++index) {
        const auto at = entries.base + (index * kRewardEntryStride);
        std::uint16_t child = kNoRewardSelector;
        std::uint8_t gate = 0;
        if (!read(listsBlob, at + kRewardEntryChildOffset, child)
            || !reward_class_mask(listsBlob, at, gate) || gate != kAllCharacterClasses) {
            return false;
        }
        // The +10 selector is unresolved; do not follow it as a reward-list child.
        if (child != kNoRewardSelector && !read_reward_branch(listsBlob, lists, child, pool)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Extracts and publishes the vendor catalog from the installed packages. */
bool build(const reader::Source& source,
           reader::Scratch& scratch,
           const items::packages::RewardConditions& conditions) noexcept {
    if (state::build_data::vendor_catalog_ready()) {
        return true;
    }
    static Storage storage{};
    storage.reset();
    if (!read_index(source, scratch, storage)) {
        report(storage, 0, "index");
        return false;
    }
    // Walk the index in order: the catalog requires ascending definition order. A definition that
    // will not read or will not fit costs that vendor alone, never the whole pass.
    std::size_t skipped = 0;
    for (std::size_t row = 0; row < storage.indexCount; ++row) {
        const domain::IndexEntry entry = storage.index[row];
        if (read_definition(conditions, source, scratch, entry, storage)) {
            continue;
        }
        ++skipped;
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=build_data stage=vendors result=skip hash=0x%08X row=%zu "
                          "definitions=%zu sale=%zu",
                          entry.definitionHash,
                          row,
                          storage.definitionCount,
                          storage.saleRowCount);
    }
    const bool published = state::build_data::publish_vendor_catalog(
        std::span(storage.index).first(storage.indexCount),
        std::span(storage.definitions).first(storage.definitionCount),
        std::span(storage.saleRows).first(storage.saleRowCount),
        std::span(storage.installedRows).first(storage.installedRowCount),
        std::span(storage.transferRules).first(storage.transferRuleCount));
    report(storage, skipped, published ? "ok" : "publish");
    return published;
}

/**
 * Publishes the supported nested faction reward pools from the installed investment root.
 * @param source Installed package directory and borrowed keys.
 * @param scratch Shared package block storage.
 * @param root Installed investment-root bytes.
 * @param itemTable Installed item index table bytes.
 * @param itemRows Item index array within itemTable.
 * @return False when any supported package pool is unreadable or invalid.
 */
bool build_rewards(const reader::Source& source,
                   reader::Scratch& scratch,
                   std::span<const std::byte> root,
                   std::span<const std::byte> itemTable,
                   const tables::Array& itemRows) noexcept {
    if (rewards::ready()) {
        return true;
    }
    std::uint32_t tag = 0;
    static std::vector<std::byte> listsBlob{};
    static std::vector<std::byte> definition{};
    tables::Array lists{};
    if (!state::build_data::item_definitions_ready()
        || !state::build_data::configured_item_details_ready()
        || !tables::slot_tag(root, kRewardListTableSlot, tag) || tag == 0
        || !reader::read_tag(source, scratch, tag, listsBlob)
        || !tables::find_array_at(listsBlob, tables::kTableArrayDescriptor, lists)
        || lists.elementClass != kRewardListRowClass
        || lists.count > (std::numeric_limits<std::uint16_t>::max)()
        || lists.dataOffset > listsBlob.size()
        || lists.count > (listsBlob.size() - lists.dataOffset) / kRewardListRowStride) {
        return false;
    }
    std::array<rewards::Pool, rewards::kPackageHashes.size()> pools{};
    for (std::size_t index = 0; index < pools.size(); ++index) {
        if (!read_reward_pool(source,
                              scratch,
                              itemTable,
                              itemRows,
                              listsBlob,
                              lists,
                              rewards::kPackageHashes[index],
                              pools[index],
                              definition)) {
            core::log::writef(core::log::Channel::state,
                              core::log::Level::warn,
                              "ev=build_data stage=vendor_rewards result=skip package=0x%08X",
                              rewards::kPackageHashes[index]);
            return false;
        }
    }
    const bool published = rewards::replace(pools);
    core::log::writef(
        core::log::Channel::state,
        published ? core::log::Level::info : core::log::Level::warn,
        "ev=build_data stage=vendor_rewards result=%s vanguard=%zu crucible=%zu gunsmith=%zu",
        published ? "ok" : "invalid",
        pools[0].counts[0],
        pools[1].counts[0],
        pools[2].counts[0]);
    return published;
}

} // namespace sunrise::client::content::vendors
