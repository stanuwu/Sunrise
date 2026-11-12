#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../middleware/content/packages/schema/generated/tag_classes_808074.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808075.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808078.h"

namespace sunrise::client::content::vendors {

namespace generated = middleware::content::packages::schema::generated;

/** Tag of the installed vendor index blob, which names every vendor definition. */
inline constexpr std::uint32_t kIndexRootTag = 0x8131931DU;
/** Installed faction table named by the build-86657 investment content. */
inline constexpr std::uint32_t kFactionTableTag = 0x81327CD5U;
/** The faction table's rows begin at this array descriptor. */
inline constexpr std::size_t kFactionArrayDescriptor = offsetof(generated::C8080759b, table);
/** One faction row holds its hash and progression index in the first eight bytes. */
inline constexpr std::size_t kFactionRowStride = sizeof(generated::C808074ce);
/** The installed faction table's row class. */
inline constexpr std::uint32_t kFactionRowClass = generated::C808074ce::kClass;
/** Faction definition hash at the start of a faction row. */
inline constexpr std::size_t kFactionHashOffset = offsetof(generated::C808074ce, unk0);
/** Progression definition index following the faction hash. */
inline constexpr std::size_t kFactionProgressionIndexOffset = offsetof(generated::C808074ce, gap4);
/** The last halfword before resetInterval selects the vendor's faction row. */
inline constexpr std::size_t kVendorFactionIndexOffset =
    offsetof(generated::SInvestmentVendor, resetInterval) - sizeof(std::uint16_t);

/** Array descriptors of a vendor definition: category rows, sale rows and interactions. */
inline constexpr std::size_t kInstalledArrayDescriptor =
    offsetof(generated::SInvestmentVendor, categoryList);
inline constexpr std::size_t kSaleArrayDescriptor =
    offsetof(generated::SInvestmentVendor, itemList);
inline constexpr std::size_t kInteractionArrayDescriptor =
    offsetof(generated::SInvestmentVendor, interactions);
/** Direct availability expression and category selector of a vendor interaction. */
inline constexpr std::size_t kInteractionConditionField = offsetof(generated::C80807857, h90b11fae);
inline constexpr std::size_t kInteractionCategoryOffset = offsetof(generated::C80807857, gap38);
/** Reply rows carry their own conditional reward expression. */
inline constexpr std::size_t kInteractionReplyField = offsetof(generated::C80807857, replies);
/** Sale admission and selection each have a list of conditions. */
inline constexpr std::size_t kSaleAdmissionField = offsetof(generated::SVendorSale, array8);
inline constexpr std::size_t kSaleSelectionField = offsetof(generated::SVendorSale, array120);
/** A vendor definition's transfer rule array descriptor. */
inline constexpr std::size_t kTransferArrayDescriptor =
    offsetof(generated::SInvestmentVendor, additionalPolicy96);
/** Raw reset interval. Its unit, epoch and scope are open, so it is stored unconverted. */
inline constexpr std::size_t kResetIntervalOffset =
    offsetof(generated::SInvestmentVendor, resetInterval);
/** Raw reset phase, paired with the interval. */
inline constexpr std::size_t kResetPhaseOffset = offsetof(generated::SInvestmentVendor, resetPhase);

/** Sale row cost array descriptor, which is what the row charges. */
inline constexpr std::size_t kSaleCostArrayDescriptor = offsetof(generated::SVendorSale, costs);
/** Cost entry item-definition index. */
inline constexpr std::size_t kSaleCostItemIndexOffset =
    offsetof(generated::SVendorCost, costItemIndex);
/** Cost entry static quantity. */
inline constexpr std::size_t kSaleCostQuantityOffset = offsetof(generated::SVendorCost, quantity);
/** Cost entry descriptor of its first expression array. An entry carrying one is not static. */
inline constexpr std::size_t kSaleCostFirstProgramDescriptor =
    offsetof(generated::SVendorCost, hA170ebf3);
/** Cost entry descriptor of its second expression array. An entry carrying one is not static. */
inline constexpr std::size_t kSaleCostSecondProgramDescriptor =
    offsetof(generated::SVendorCost, hDff8ca81);
/** Cost entry trailing word, which every expression-free entry carries as `kPlainCostWord`. */
inline constexpr std::size_t kSaleCostWordOffset = offsetof(generated::SVendorCost, scale);
/**
 * Sale row availability conditions, every one of which must hold for a purchase. A Store row that
 * sells an item once carries one condition here: its acquired flag is not set.
 */
inline constexpr std::size_t kSaleAvailabilityDescriptor = offsetof(generated::SVendorSale, array8);
/** Element class of the availability list; each row is one expression field. */
inline constexpr std::uint32_t kSaleAvailabilityRowClass = generated::RewardCondition::kClass;
/** Sale row main item-definition index. */
inline constexpr std::size_t kSaleItemIndexOffset = offsetof(generated::SVendorSale, saleItemIndex);
/** Sale row vendor category index. */
inline constexpr std::size_t kSaleCategoryIndexOffset =
    offsetof(generated::SVendorSale, categoryIndex);
/** Sale row secondary item-definition index. */
inline constexpr std::size_t kSaleSecondaryItemOffset =
    offsetof(generated::SVendorSale, secondaryItem);
/** A category row names its item by definition hash at this offset. */
inline constexpr std::size_t kInstalledRowHashOffset =
    offsetof(generated::VendorInstalledRow, itemHash);

} // namespace sunrise::client::content::vendors
