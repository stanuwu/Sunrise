#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::vendors {

/** Rows of the installed vendor index. The live table has 511. */
inline constexpr std::size_t kIndexCapacity = 512;
/** Every index row gets a definition, so the two capacities are the same. */
inline constexpr std::size_t kDefinitionCapacity = kIndexCapacity;
/** Sale rows across every definition. The live total is 15,768 and one vendor declares 1,044. */
inline constexpr std::size_t kSaleRowCapacity = 16'384;
/** Category rows across every definition. The live total is 2,479 and one vendor declares 129. */
inline constexpr std::size_t kInstalledRowCapacity = 4'096;

/** One vendor index row is 24 bytes: hash at +0, definition tag at +16. */
inline constexpr std::size_t kIndexRowStride = 24;
/** One sale row is 184 bytes. */
inline constexpr std::size_t kSaleRowStride = 184;
/** One installed row is 24 bytes. */
inline constexpr std::size_t kInstalledRowStride = 24;
/** One interaction row is 80 bytes. */
inline constexpr std::size_t kThirdRowStride = 80;
/** One cost entry is 48 bytes. `SaleCost` documents its layout. */
inline constexpr std::size_t kSaleCostRowStride = 48;
/** Element class of a sale row's cost array. */
inline constexpr std::uint32_t kSaleCostRowClass = 0x80807865U;

/** Wrapper class of the vendor index blob. */
inline constexpr std::uint32_t kIndexWrapperClass = 0x8080784AU;
/** Element class of the vendor index array. */
inline constexpr std::uint32_t kIndexRowClass = 0x8080784EU;
/** Class of a vendor definition blob. */
inline constexpr std::uint32_t kDefinitionClass = 0x80807850U;
/** Element class of a definition's installed array. */
inline constexpr std::uint32_t kInstalledRowClass = 0x80807860U;
/** Element class of a definition's sale array. */
inline constexpr std::uint32_t kSaleRowClass = 0x80807861U;

/** Sale row +176 carries this when the row names no secondary item. */
inline constexpr std::uint16_t kAbsentSecondaryItem = 0xFFFFU;
/** Sale row +100 carries this when the row belongs to no category. The client tests for it. */
inline constexpr std::int32_t kAbsentCategoryIndex = -1;

/** One row of the installed vendor index, which maps a vendor hash to its definition tag. */
struct IndexEntry {
    std::uint32_t definitionHash{};
    std::uint32_t definitionTag{};
    /** Row position, which is the index the opcode-901 request carries. */
    std::uint16_t index{};
};

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
    /** First row of the unnamed third array, as an offset into the definition blob. */
    std::uint32_t thirdRowBase{};
    std::uint32_t thirdRowClass{};
    /** First row of this definition's range in the flat sale bank. */
    std::uint32_t saleRowOffset{};
    /** First row of this definition's range in the flat installed bank. */
    std::uint32_t installedRowOffset{};
    /** Raw definition +20. Its unit, epoch and scope are open, so it is not converted. */
    std::uint32_t resetIntervalRaw{};
    /** Raw definition +24, paired with the interval and equally open. */
    std::uint32_t resetPhaseRaw{};
    /** Row of the vendor index this definition is named by. */
    std::uint16_t index{};
    std::uint16_t installedCount{};
    std::uint16_t saleCount{};
    std::uint16_t thirdCount{};
};

/** A cost entry naming no item carries this. */
inline constexpr std::uint16_t kAbsentCostItem = 0xFFFFU;
/** Cost entries one sale row may declare. The widest row in the installed catalog declares four. */
inline constexpr std::size_t kSaleCostCapacity = 4;
/**
 * Cost entry +40 in every expression-free entry of the installed catalog. Its role is not
 * decoded, so an entry carrying anything else is conditional rather than priced statically.
 */
inline constexpr std::uint32_t kPlainCostWord = 100'000U;

/**
 * One cost entry of a sale row (row +32 array, `kSaleCostRowClass`, 48 bytes), reduced to its
 * static item and quantity. The entry also carries two expression arrays, at +8 and +24, and a
 * word at +40; those decide whether the static quantity is the price at all, and the answer is
 * kept on the row as its `PriceState`. On Xûr's definition every entry is static: item 128 with
 * 29, 23, 97 and 9 units, the Legendary Shard prices of his weapons, armour, Fated Engram and
 * Invitation of the Nine.
 */
struct SaleCost {
    /** Entry +0. Cost item-definition index. */
    std::uint16_t itemIndex{kAbsentCostItem};
    /** Entry +4. Units charged. */
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

/** One sale row of one vendor definition. */
struct SaleRow {
    /** Row +100. The row's vendor category. The catalog bounds it by the category count. */
    std::int32_t categoryIndex{};
    /** Row +70. Main sale item-definition index. */
    std::uint16_t itemIndex{};
    /** Row +176. `kAbsentSecondaryItem` when the row names none. */
    std::uint16_t secondaryItemIndex{};
    /** Every cost entry, in declared order. A plain row charges all of them together. */
    std::array<SaleCost, kSaleCostCapacity> costs{};
    /** Entries of `costs` in use. Zero when the row charges nothing. */
    std::uint8_t costCount{};
    /** Whether `costs` is the price. Only a plain row is charged. */
    PriceState priceState{PriceState::plain};
};

/** @return The static cost entries of one sale row, which are its price only while it is plain. */
[[nodiscard]] inline std::span<const SaleCost> cost_entries(const SaleRow& row) noexcept {
    return {row.costs.data(), row.costCount > row.costs.size() ? std::size_t{0} : row.costCount};
}

/** One category row, reduced to the definition hash a rowless request resolves through. */
struct InstalledRow {
    std::uint32_t definitionHash{};
};

} // namespace sunrise::state::build_data::vendors
