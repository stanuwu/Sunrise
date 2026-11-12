#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/content/packages/schema/generated/tag_classes_808078.h"
#include "../../unlocks/unlocks_expression.h"

namespace sunrise::state::build_data::vendors {

/** Hash scans increment a signed 16-bit cursor before comparing the row count. */
inline constexpr std::size_t kIndexCapacity = 0x7FFF;
/** Every index row gets a definition, so the two capacities are the same. */
inline constexpr std::size_t kDefinitionCapacity = kIndexCapacity;
/** Sale rows across every definition. The live total is 15,768 and one vendor declares 1,044. */
inline constexpr std::size_t kSaleRowCapacity = 16'384;
/** Category rows across every definition. The live total is 2,479 and one vendor declares 129. */
inline constexpr std::size_t kInstalledRowCapacity = 4'096;

/** Row sizes of the vendor index, sale, installed, interaction and cost arrays. */
inline constexpr std::size_t kIndexRowStride =
    sizeof(middleware::content::packages::schema::generated::VendorIndexRow);
inline constexpr std::size_t kSaleRowStride =
    sizeof(middleware::content::packages::schema::generated::SVendorSale);
inline constexpr std::size_t kInstalledRowStride =
    sizeof(middleware::content::packages::schema::generated::VendorInstalledRow);
inline constexpr std::size_t kInteractionRowStride =
    sizeof(middleware::content::packages::schema::generated::C80807857);
inline constexpr std::size_t kSaleCostRowStride =
    sizeof(middleware::content::packages::schema::generated::SVendorCost);
/** Element class of a sale row's cost array. */
inline constexpr std::uint32_t kSaleCostRowClass =
    middleware::content::packages::schema::generated::SVendorCost::kClass;
/** A vendor without a resolved faction has no progression to award. */
inline constexpr std::uint16_t kUnavailableFactionProgressionIndex = 0xFFFFU;

/** Wrapper class of the vendor index blob. */
inline constexpr std::uint32_t kIndexWrapperClass =
    middleware::content::packages::schema::generated::SVendorIndex::kClass;
/** Element class of the vendor index array. */
inline constexpr std::uint32_t kIndexRowClass =
    middleware::content::packages::schema::generated::VendorIndexRow::kClass;
/** Class of a vendor definition blob. */
inline constexpr std::uint32_t kDefinitionClass =
    middleware::content::packages::schema::generated::SInvestmentVendor::kClass;
/** Element class of a definition's installed array. */
inline constexpr std::uint32_t kInstalledRowClass =
    middleware::content::packages::schema::generated::VendorInstalledRow::kClass;
/** Element class of a definition's sale array. */
inline constexpr std::uint32_t kSaleRowClass =
    middleware::content::packages::schema::generated::SVendorSale::kClass;

/** `SVendorSale::secondaryItem` carries this when the row names no secondary item. */
inline constexpr std::uint16_t kAbsentSecondaryItem = 0xFFFFU;
/** `SVendorSale::categoryIndex` carries this for a row in no category. The client tests it. */
inline constexpr std::int32_t kAbsentCategoryIndex = -1;

/** One row of the installed vendor index, which maps a vendor hash to its definition tag. */
struct IndexEntry {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    /** Row position, which is the index the opcode-901 request carries. */
    std::uint16_t index{};
};

/** Transfer rules across every definition. The shipped total is 21 and the Vault declares 15. */
inline constexpr std::size_t kTransferRuleCapacity = 256;
/** A rule with this destination returns the item to its authored bucket. */
inline constexpr std::uint8_t kAuthoredDestination = 0xFFU;

/** Accepted source bucket and destination; FF selects the item's authored bucket. */
struct TransferRule {
    std::uint8_t sourceBucket{};
    std::uint8_t destinationBucket{};
};
static_assert(sizeof(TransferRule) == 2);

/** One extracted vendor definition and the flat-bank ranges its rows occupy. */
struct Definition {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    /** Class the package entry records, which must be `kDefinitionClass`. */
    std::uint32_t definitionClass{};
    /** Definition blob size. Every array must end inside it. */
    std::uint32_t definitionSize{};
    /** First installed row, as an offset into the definition blob. */
    std::uint32_t installedRowBase{};
    std::uint32_t installedRowClass{};
    /** First sale row, as an offset into the definition blob. */
    std::uint32_t saleRowBase{};
    std::uint32_t saleRowClass{};
    /** First row of the interactions array, as an offset into the definition blob. */
    std::uint32_t interactionRowBase{};
    std::uint32_t interactionRowClass{};
    /** First row of this definition's range in the flat sale bank. */
    std::uint32_t saleRowOffset{};
    /** First row of this definition's range in the flat installed bank. */
    std::uint32_t installedRowOffset{};
    /** First row of this definition's range in the flat transfer bank. */
    std::uint32_t transferRuleOffset{};
    /** Raw `resetInterval`. Its unit, epoch and scope are open, so it is not converted. */
    std::uint32_t resetIntervalRaw{};
    /** Raw `resetPhase`, paired with the interval and equally open. */
    std::uint32_t resetPhaseRaw{};
    /** Row of the vendor index this definition is named by. */
    std::uint16_t index{};
    std::uint16_t installedCount{};
    std::uint16_t saleCount{};
    std::uint16_t interactionCount{};
    /** Index into the faction table, or an out-of-range no-faction value. */
    std::uint16_t factionIndexRaw{};
    /** Definition hash and progression selected by the faction row. */
    std::uint32_t factionHash{};
    std::uint16_t factionProgressionIndex{kUnavailableFactionProgressionIndex};
    /** Zero when the transfer array is unreadable; that disables transfer alone, not sales. */
    std::uint16_t transferRuleCount{};
};

/** A cost entry naming no item carries this. */
inline constexpr std::uint16_t kAbsentCostItem = 0xFFFFU;
/** Cost entries one sale row may declare. The widest row in the installed catalog declares four. */
inline constexpr std::size_t kSaleCostCapacity = 4;
/**
 * `SVendorCost::scale` in every expression-free entry of the installed catalog. Its role is not
 * decoded, so an entry carrying anything else is conditional rather than priced statically.
 */
inline constexpr std::uint32_t kPlainCostWord = 100'000U;

/**
 * One `SVendorSale::costs` entry, reduced to its
 * static item and quantity. Whether those are the price is the row's `PriceState`.
 */
struct SaleCost {
    /** Cost item-definition index. */
    std::uint16_t itemIndex{kAbsentCostItem};
    /** Units charged. */
    std::uint32_t quantity{};
};

/** Whether a sale row's static cost entries are its price. */
enum class PriceState : std::uint8_t {
    /** Every entry is expression-free with the plain word: the static quantities are the price. */
    plain,
    /**
     * An entry carries an expression, or a word other than `kPlainCostWord`. Its price depends
     * on state this build does not evaluate, so the row cannot be bought.
     */
    conditional,
    /** The row's cost array did not read. The row keeps its item and category and cannot be bought.
     */
    unreadable,
};

/**
 * Instructions one sale row's availability conditions expand to, joined by AND. The Store's
 * ownership gate expands to two. A longer list is kept as an unreadable row, which refuses the
 * purchase; the vendor log line counts those rows. 8 bytes each, so 1 MiB across a full bank.
 */
inline constexpr std::size_t kAvailabilityCapacity = 8;

/** One sale row of one vendor definition. */
struct SaleRow {
    /** The row's vendor category. The catalog bounds it by the category count. */
    std::int32_t categoryIndex{};
    /** Main sale item-definition index. */
    std::uint16_t itemIndex{};
    /** `kAbsentSecondaryItem` when the row names none. */
    std::uint16_t secondaryItemIndex{};
    /** Every cost entry, in declared order. A plain row charges all of them together. */
    std::array<SaleCost, kSaleCostCapacity> costs{};
    /** Entries of `costs` in use. Zero when the row charges nothing. */
    std::uint8_t costCount{};
    /** Whether `costs` is the price. Only a plain row is charged. */
    PriceState priceState{PriceState::plain};
    /** Conditions that must all hold for the row to be bought, bound to the saved banks. */
    std::array<unlocks::Instruction, kAvailabilityCapacity> availability{};
    /** Entries of `availability` in use. Zero when the row is always available. */
    std::uint8_t availabilityCount{};
    /** True when the conditions did not read, bind or fit, which refuses the row. */
    bool availabilityUnreadable{};
};

/** @return The static cost entries of one sale row; the catalog bounds the count on install. */
[[nodiscard]] inline std::span<const SaleCost> cost_entries(const SaleRow& row) noexcept {
    return {row.costs.data(), static_cast<std::size_t>(row.costCount)};
}

/** One category row, reduced to the definition hash a rowless request resolves through. */
struct InstalledRow {
    std::uint32_t definitionHash{};
};

} // namespace sunrise::state::build_data::vendors
