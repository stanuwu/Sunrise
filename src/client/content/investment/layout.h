#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::content::investment::layout {

/** The globals blob stores its investment-root tag after 16 ABI bytes. */
inline constexpr std::size_t kGlobalsRootTagOffset = 16;
/** The runtime bucket-table handle sits at byte 280 of the investment root. */
inline constexpr std::size_t kInventoryBucketTableTagOffset = 280;
/** The dense item-table handle sits at byte 776 of the investment root. */
inline constexpr std::size_t kItemTableTagOffset = 776;
/** The socket-entry-list table handle sits at byte 1,560 of the investment root. */
inline constexpr std::size_t kSocketEntryListTableTagOffset = 1560;

/** Fixed prefix of the loaded investment-globals blob. */
struct InvestmentGlobals {
    std::array<std::byte, kGlobalsRootTagOffset> opaqueBeforeInvestmentRootTag{};
    std::uint32_t investmentRootTag{};
};

/**
 * Fixed investment-root prefix shared by the numeric runtime-table extractors.
 * One layout for the three verified handles stops modules quietly giving the same native prefix
 * different meanings.
 */
struct InvestmentRoot {
    std::array<std::byte, kInventoryBucketTableTagOffset> opaqueBeforeInventoryBucketTableTag{};
    std::uint32_t inventoryBucketTableTag{};
    std::array<std::byte,
               kItemTableTagOffset - kInventoryBucketTableTagOffset - sizeof(std::uint32_t)>
        opaqueBeforeItemTableTag{};
    std::uint32_t itemTableTag{};
    std::array<std::byte,
               kSocketEntryListTableTagOffset - kItemTableTagOffset - sizeof(std::uint32_t)>
        opaqueBeforeSocketEntryListTableTag{};
    std::uint32_t socketEntryListTableTag{};
};

static_assert(offsetof(InvestmentGlobals, investmentRootTag) == kGlobalsRootTagOffset);
static_assert(offsetof(InvestmentRoot, inventoryBucketTableTag) == kInventoryBucketTableTagOffset);
static_assert(offsetof(InvestmentRoot, itemTableTag) == kItemTableTagOffset);
static_assert(offsetof(InvestmentRoot, socketEntryListTableTag) == kSocketEntryListTableTagOffset);

} // namespace sunrise::client::content::investment::layout
