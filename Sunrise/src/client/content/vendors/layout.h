#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::client::content::vendors {

/** Tag of the installed vendor index blob, which names every vendor definition. */
inline constexpr std::uint32_t kIndexRootTag = 0x8131931DU;

/** A vendor definition holds its installed array descriptor here. */
inline constexpr std::size_t kInstalledArrayDescriptor = 32;
/** A vendor definition holds its sale array descriptor here. */
inline constexpr std::size_t kSaleArrayDescriptor = 48;
/** A vendor definition holds its unnamed third array descriptor here. */
inline constexpr std::size_t kThirdArrayDescriptor = 80;
/** Raw reset interval. Its unit, epoch and scope are open, so it is stored unconverted. */
inline constexpr std::size_t kResetIntervalOffset = 20;
/** Raw reset phase, paired with the interval. */
inline constexpr std::size_t kResetPhaseOffset = 24;

/** Sale row cost array descriptor, which is what the row charges. */
inline constexpr std::size_t kSaleCostArrayDescriptor = 32;
/** Cost entry item-definition index. */
inline constexpr std::size_t kSaleCostItemIndexOffset = 0;
/** Cost entry static quantity. */
inline constexpr std::size_t kSaleCostQuantityOffset = 4;
/** Cost entry descriptor of its first expression array. An entry carrying one is not static. */
inline constexpr std::size_t kSaleCostFirstProgramDescriptor = 8;
/** Cost entry descriptor of its second expression array. An entry carrying one is not static. */
inline constexpr std::size_t kSaleCostSecondProgramDescriptor = 24;
/** Cost entry trailing word, which every expression-free entry carries as `kPlainCostWord`. */
inline constexpr std::size_t kSaleCostWordOffset = 40;
/** Sale row main item-definition index. */
inline constexpr std::size_t kSaleItemIndexOffset = 70;
/** Sale row vendor category index. */
inline constexpr std::size_t kSaleCategoryIndexOffset = 100;
/** Sale row secondary item-definition index. */
inline constexpr std::size_t kSaleSecondaryItemOffset = 176;
/** A category row names its item by definition hash at this offset. */
inline constexpr std::size_t kInstalledRowHashOffset = 0;

} // namespace sunrise::client::content::vendors
