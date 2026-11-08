#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../middleware/content/packages/schema/generated/tag_classes_808078.h"

namespace sunrise::client::content::vendors {

namespace generated = middleware::content::packages::schema::generated;

/** Tag of the installed vendor index blob, which names every vendor definition. */
inline constexpr std::uint32_t kIndexRootTag = 0x8131931DU;

/** Array descriptors of a vendor definition: category rows, sale rows and interactions. */
inline constexpr std::size_t kInstalledArrayDescriptor =
    offsetof(generated::SInvestmentVendor, categoryList);
inline constexpr std::size_t kSaleArrayDescriptor =
    offsetof(generated::SInvestmentVendor, itemList);
inline constexpr std::size_t kInteractionArrayDescriptor =
    offsetof(generated::SInvestmentVendor, interactions);
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
