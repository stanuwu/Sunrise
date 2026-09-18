#include "codec.h"

namespace sunrise::state::build_data::cache::records {

/** Encodes one vendor index row. */
bool encode(const vendors::IndexEntry& value, VendorIndexRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.definitionTag = value.definitionTag;
    record.index = value.index;
    return true;
}

/** Decodes one vendor index row. */
bool decode(const VendorIndexRecord& record, vendors::IndexEntry& value) noexcept {
    value = {};
    if (record.reserved != 0) {
        return false;
    }
    value = {record.definitionHash, record.definitionTag, record.index};
    return true;
}

/** Encodes one vendor definition and its flat-bank ranges. */
bool encode(const vendors::Definition& value, VendorDefinitionRecord& record) noexcept {
    record = {};
    record.definitionHash = value.definitionHash;
    record.definitionTag = value.definitionTag;
    record.definitionClass = value.definitionClass;
    record.definitionSize = value.definitionSize;
    record.installedRowBase = value.installedRowBase;
    record.installedRowClass = value.installedRowClass;
    record.saleRowBase = value.saleRowBase;
    record.saleRowClass = value.saleRowClass;
    record.thirdRowBase = value.thirdRowBase;
    record.thirdRowClass = value.thirdRowClass;
    record.saleRowOffset = value.saleRowOffset;
    record.installedRowOffset = value.installedRowOffset;
    record.resetIntervalRaw = value.resetIntervalRaw;
    record.resetPhaseRaw = value.resetPhaseRaw;
    record.index = value.index;
    record.installedCount = value.installedCount;
    record.saleCount = value.saleCount;
    record.thirdCount = value.thirdCount;
    return true;
}

/** Decodes one vendor definition and its flat-bank ranges. */
bool decode(const VendorDefinitionRecord& record, vendors::Definition& value) noexcept {
    value = {};
    // The catalog checks every range against the whole domain. Only the class is checked here.
    // A row of another class is not a vendor definition, whatever its ranges say.
    if (record.definitionClass != vendors::kDefinitionClass) {
        return false;
    }
    value.definitionHash = record.definitionHash;
    value.definitionTag = record.definitionTag;
    value.definitionClass = record.definitionClass;
    value.definitionSize = record.definitionSize;
    value.installedRowBase = record.installedRowBase;
    value.installedRowClass = record.installedRowClass;
    value.saleRowBase = record.saleRowBase;
    value.saleRowClass = record.saleRowClass;
    value.thirdRowBase = record.thirdRowBase;
    value.thirdRowClass = record.thirdRowClass;
    value.saleRowOffset = record.saleRowOffset;
    value.installedRowOffset = record.installedRowOffset;
    value.resetIntervalRaw = record.resetIntervalRaw;
    value.resetPhaseRaw = record.resetPhaseRaw;
    value.index = record.index;
    value.installedCount = record.installedCount;
    value.saleCount = record.saleCount;
    value.thirdCount = record.thirdCount;
    return true;
}

/** Encodes one vendor sale row. Unused cost entries stay zero so the packed row always matches. */
bool encode(const vendors::SaleRow& value, VendorSaleRowRecord& record) noexcept {
    record = {};
    if (value.costCount > value.costs.size()) {
        return false;
    }
    record.itemIndex = value.itemIndex;
    record.secondaryItemIndex = value.secondaryItemIndex;
    record.categoryIndex = value.categoryIndex;
    record.costCount = value.costCount;
    record.priceState = static_cast<std::uint8_t>(value.priceState);
    for (std::size_t cost = 0; cost < value.costCount; ++cost) {
        record.costs[cost].itemIndex = value.costs[cost].itemIndex;
        record.costs[cost].quantity = value.costs[cost].quantity;
    }
    return true;
}

/** Decodes one vendor sale row. */
bool decode(const VendorSaleRowRecord& record, vendors::SaleRow& value) noexcept {
    value = {};
    if (record.reserved != decltype(record.reserved){} || record.costCount > record.costs.size()
        || record.priceState > static_cast<std::uint8_t>(vendors::PriceState::unreadable)) {
        return false;
    }
    for (std::size_t cost = 0; cost < record.costs.size(); ++cost) {
        const VendorSaleCostRecord& stored = record.costs[cost];
        if (stored.reserved != 0) {
            return false;
        }
        if (cost >= record.costCount) {
            if (stored.itemIndex != 0 || stored.quantity != 0) {
                return false;
            }
            continue;
        }
        value.costs[cost] = {stored.itemIndex, stored.quantity};
    }
    value.itemIndex = record.itemIndex;
    value.secondaryItemIndex = record.secondaryItemIndex;
    value.categoryIndex = record.categoryIndex;
    value.costCount = record.costCount;
    value.priceState = static_cast<vendors::PriceState>(record.priceState);
    return true;
}

/** Encodes one vendor category row. */
bool encode(const vendors::InstalledRow& value, VendorInstalledRowRecord& record) noexcept {
    record = {value.definitionHash};
    return true;
}

/** Decodes one vendor category row. */
bool decode(const VendorInstalledRowRecord& record, vendors::InstalledRow& value) noexcept {
    value = {record.definitionHash};
    return true;
}

} // namespace sunrise::state::build_data::cache::records
