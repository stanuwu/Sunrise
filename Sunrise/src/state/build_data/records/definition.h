#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::state::build_data::records {

/** The shipped build declares 2242 records and lore entries. The domain leaves room above that. */
inline constexpr std::size_t kDefinitionCapacity = 4096;

/**
 * Account value bank row that holds Triumph Score.
 *
 * Found by authoring every account value slot to `100000 + its own row` and reading the score back
 * as 102115. It is a plain replicated value, not a progression and not derived by the client, so a
 * server that wants a score has to total one itself.
 */
inline constexpr std::uint16_t kTriumphScoreValueIndex = 2115U;

/** A record whose completion flag no mapping table addresses carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableFlagIndex = 0xFFFFU;

/** A record naming no category value slot carries this instead of an index. */
inline constexpr std::uint16_t kUnavailableValueIndex = 0xFFFFU;

/**
 * One record reduced to what a claim needs.
 *
 * A record row carries the unlock slot of its completion flag. A slot is not an array index: the
 * byte that feeds it lives at the row number of the mapping table whose destination is that slot.
 * The index is resolved once here, at extraction, so a claim never has to walk the mapping tables.
 */
struct Definition {
    /** Native record row, which is what an opcode-1801 claim names. */
    std::uint16_t definitionIndex{};
    /** Account flag bank mapping row, or kUnavailableFlagIndex when the slot is unaddressable. */
    std::uint16_t completionFlagIndex{kUnavailableFlagIndex};
    /** Points this record is worth, which the shipped table keeps at 500 or below. */
    std::uint16_t scoreValue{};
    /**
     * Account value index of the category this record names, or kUnavailableValueIndex.
     *
     * Only a category's parent record names its category's own slot, so this is what distinguishes
     * the parent from the chapters beneath it. The parent is excluded from its own progress bar.
     */
    std::uint16_t categoryValueIndex{kUnavailableValueIndex};
};

} // namespace sunrise::state::build_data::records
