#include "reward_resolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../build_data/runtime.h"
#include "../investment/store.h"
#include "../unlocks/unlocks_expression.h"

namespace sunrise::state::reward_resolution {
namespace {

namespace definitions = build_data::rewards;
using AcquiredFlags = std::bitset<unlocks::kAccountFlagCapacity>;

/** Completed acquisitions are visible to later draws without changing saved unlocks. */
struct ConditionInputs {
    const Context& context;
    const AcquiredFlags* acquiredFlags{};
};

/** The empty category tag matches every wrapper selection. */
constexpr std::uint32_t kEmptyTag = 0x811C9DC5U;
/** Native wrappers retain up to 64 selections, including rows without an item grant. */
constexpr std::size_t kDrawCapacity = 64;
/** A grant quantity is published as a signed 32-bit stack size. */
constexpr auto kMaximumQuantity =
    static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());

bool refuse(const Context& context, const char* reason) noexcept {
    if (context.refusal != nullptr) {
        *context.refusal = reason;
    }
    return false;
}

/**
 * Reads one row from the caller's copy of the saved banks.
 * @param bank A bank investment::store::saved_bank names for an instruction.
 * @param index A row the instruction's validation placed inside that bank.
 * @return The row's value; a flag row reads its stored byte.
 */
std::int32_t saved_row(const unlocks::Table& banks,
                       investment::store::Bank bank,
                       std::uint32_t index) noexcept {
    switch (bank) {
    case investment::store::Bank::accountFlags:
        return banks.accountFlags[index];
    case investment::store::Bank::profileFlags:
        return banks.profileFlags[index];
    case investment::store::Bank::objectiveValues:
        return banks.objectiveValues[index];
    case investment::store::Bank::characterObjectFlags:
        return banks.characterObjectFlags[index];
    case investment::store::Bank::characterObjectValues:
        return banks.characterObjectValues[index];
    default:
        return 0;
    }
}

/**
 * Reads one validated flag from the caller's unlock banks or selected character.
 * A flag this draw already acquired reads as set before any bank is saved.
 * @param raw The ConditionInputs of the evaluation.
 * @param set Receives the flag.
 * @return False for a bank the saved state does not hold.
 */
bool read_flag(const void* raw, const unlocks::Instruction& instruction, bool& set) noexcept {
    const auto& inputs = *static_cast<const ConditionInputs*>(raw);
    const auto& context = inputs.context;
    const auto index = instruction.operand;
    if (instruction.bank == unlocks::Bank::characterClass) {
        set = index == static_cast<std::uint32_t>(context.characterClass);
        return true;
    }
    investment::store::Bank bank{};
    if (!investment::store::saved_bank(instruction.bank, true, bank)) {
        return false;
    }
    const bool acquired = bank == investment::store::Bank::accountFlags
                          && inputs.acquiredFlags != nullptr && (*inputs.acquiredFlags)[index];
    set = acquired || saved_row(context.unlocks, bank, index) == unlocks::kFlagSet;
    return true;
}

/**
 * Reads one validated value from the caller's unlock banks.
 * @param raw The ConditionInputs of the evaluation.
 * @param value Receives the value.
 * @return False for a bank that holds no values.
 */
bool read_value(const void* raw,
                const unlocks::Instruction& instruction,
                std::int32_t& value) noexcept {
    const auto& context = static_cast<const ConditionInputs*>(raw)->context;
    investment::store::Bank bank{};
    if (!investment::store::saved_bank(instruction.bank, false, bank)) {
        return false;
    }
    value = saved_row(context.unlocks, bank, instruction.operand);
    return true;
}

/**
 * Evaluates one bound program to its final value; external identities are not in the saved banks.
 * @param acquiredFlags Flags this draw acquired so far, or null outside a draw.
 * @return False for a malformed program or an external read; the refusal names which.
 */
bool evaluate_program(std::span<const definitions::Instruction> program,
                      const Context& context,
                      std::int32_t& result,
                      const AcquiredFlags* acquiredFlags = nullptr) noexcept {
    for (const auto& instruction : program) {
        if (!unlocks::valid(instruction)) {
            return refuse(context, "condition_shape");
        }
        if (instruction.bank == unlocks::Bank::external) {
            return refuse(context,
                          instruction.opcode == unlocks::Opcode::flag ? "external_flag"
                                                                      : "external_value");
        }
    }
    const ConditionInputs conditionInputs{context, acquiredFlags};
    const unlocks::Inputs inputs{read_flag, read_value, &conditionInputs};
    return unlocks::evaluate(program, inputs, result) || refuse(context, "condition_shape");
}

/**
 * Evaluates one bound condition program. An empty program is unconditional, and any other holds
 * when it evaluates to nonzero.
 * @param result Receives whether the condition holds.
 */
bool condition(std::span<const definitions::Instruction> program,
               const Context& context,
               bool& result,
               const AcquiredFlags* acquiredFlags = nullptr) noexcept {
    result = program.empty();
    if (result) {
        return true;
    }
    std::int32_t value = 0;
    if (!evaluate_program(program, context, value, acquiredFlags)) {
        return false;
    }
    result = value != 0;
    return true;
}

/** Evaluates the condition one bank range names; a range outside the bank is refused. */
bool condition(definitions::View data,
               definitions::Range expression,
               const Context& context,
               bool& result,
               const AcquiredFlags& acquiredFlags) noexcept {
    if (!definitions::fits(expression, data.instructions)) {
        result = false;
        return refuse(context, "condition_shape");
    }
    return condition(data.instructions.subspan(expression.first, expression.count),
                     context,
                     result,
                     &acquiredFlags);
}

/** SplitMix64 supplies replayable wrapper seeds and wrapper-local draws. */
std::uint64_t next_random(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    auto bits = state;
    bits = (bits ^ (bits >> 30)) * 0xBF58476D1CE4E5B9ULL;
    bits = (bits ^ (bits >> 27)) * 0x94D049BB133111EBULL;
    return bits ^ (bits >> 31);
}

/** A uniform fraction keeps the top 53 bits of a draw, the precision of a double. */
constexpr unsigned kFractionShift = 11;
/** 2^-53 scales those 53 bits into [0, 1). */
constexpr double kFractionScale = 0x1.0p-53;

/**
 * Draws for one wrapper. Each wrapper owns its selection history and random stream; nested pools
 * share them, and nested wrappers share the seed chain and the acquired flags.
 */
struct Resolver {
    definitions::View data;
    const Context& context;
    Result& result;
    std::uint64_t random;
    std::uint64_t& wrapperSeed;
    AcquiredFlags& acquiredFlags;
    bool& excludedByUnlocks;
    bool& leafDrawn;
    bool& leafFlagAcquired;
    Resolution& failure;
    std::size_t wrapperDepth{};
    std::array<const definitions::Entry*, kDrawCapacity> selectedEntries{};
    std::array<std::uint32_t, kDrawCapacity> selectedCategories{};
    std::size_t selectedCount{};

    /** @return The next uniform fraction in [0, 1) from this wrapper's stream. */
    double fraction() noexcept {
        return static_cast<double>(next_random(random) >> kFractionShift) * kFractionScale;
    }

    /**
     * Weighs one entry for a category draw: zero when it does not match, was drawn already, or
     * its condition fails; otherwise its first matching modifier's weight or its own.
     * @param depth Nesting of the pool the entry sits in.
     * @param output Receives the weight; zero means the entry cannot be drawn.
     * @return False when the entry or a nested pool is malformed.
     */
    bool weight(const definitions::Entry& entry,
                std::uint32_t category,
                std::size_t depth,
                double& output) noexcept {
        output = 0;
        if (entry.categoryHash != kEmptyTag && entry.categoryHash != category) {
            return true;
        }
        // Draws exclude the selected pool row within its category, not every copy of its item.
        for (std::size_t i = 0; i < selectedCount; ++i) {
            if (selectedEntries[i] == &entry && selectedCategories[i] == category) {
                return true;
            }
        }
        bool enabled = false;
        if (!condition(data, entry.condition, context, enabled, acquiredFlags)) {
            return false;
        }
        if (!enabled) {
            excludedByUnlocks = true;
            return true;
        }
        double value = entry.weight;
        if (!definitions::fits(entry.modifiers, data.modifiers)) {
            return refuse(context, "modifier_range");
        }
        for (const auto& modifier :
             data.modifiers.subspan(entry.modifiers.first, entry.modifiers.count)) {
            if (!condition(data, modifier.condition, context, enabled, acquiredFlags)) {
                return false;
            }
            if (enabled) {
                if (modifier.valueIndex != definitions::kAbsent) {
                    return refuse(context, "indexed_weight");
                }
                // The first matching modifier replaces the weight; a negative value keeps it.
                if (modifier.value >= 0) {
                    value = modifier.value;
                }
                break;
            }
        }
        if (!std::isfinite(value) || value < 0) {
            return refuse(context, "weight_shape");
        }
        if (value == 0) {
            // A modifier can change this weight when the unlocks change.
            excludedByUnlocks |= entry.modifiers.count != 0;
            return true;
        }
        if (entry.poolIndex != definitions::kAbsent) {
            double total = 0;
            if (!pool_weight(entry.poolIndex, category, depth + 1, total)) {
                return false;
            }
            if (total == 0) {
                return true;
            }
        } else if (entry.itemIndex != definitions::kAbsent) {
            if (entry.itemIndex >= data.items.size()) {
                return refuse(context, "item_range");
            }
        } else if (entry.supplementalIndex == definitions::kAbsent) {
            return refuse(context, "reward_target");
        }
        output = value;
        return true;
    }

    /**
     * Sums the weights of one pool's entries for a category draw.
     * @param total Receives the sum; zero means nothing in the pool can be drawn.
     * @return False when the pool is too deep, out of range or malformed.
     */
    bool pool_weight(std::uint16_t index,
                     std::uint32_t category,
                     std::size_t depth,
                     double& total) noexcept {
        total = 0;
        if (depth >= definitions::kTraversalDepth) {
            return refuse(context, "pool_depth");
        }
        if (index >= data.pools.size()) {
            return refuse(context, "pool_range");
        }
        const auto range = data.pools[index].entries;
        if (!definitions::fits(range, data.entries)) {
            return refuse(context, "entry_range");
        }
        for (const auto& entry : data.entries.subspan(range.first, range.count)) {
            double value = 0;
            if (!weight(entry, category, depth, value)) {
                return false;
            }
            total += value;
        }
        return std::isfinite(total) || refuse(context, "weight_shape");
    }

    /**
     * Draws one entry by weight and follows it to one leaf: a nested pool draws again, an item
     * is acquired, and a row with no item only counts as drawn.
     * @param total Weight sum pool_weight returned for the same pool and category.
     * @return False when the draw is refused; failure then says whether a bound was exceeded.
     */
    bool
    draw(std::uint16_t index, std::uint32_t category, std::size_t depth, double total) noexcept {
        const auto range = data.pools[index].entries;
        double remaining = fraction() * total;
        const definitions::Entry* chosen = nullptr;
        for (const auto& entry : data.entries.subspan(range.first, range.count)) {
            double value = 0;
            if (!weight(entry, category, depth, value)) {
                return false;
            }
            if (value == 0) {
                continue;
            }
            chosen = &entry;
            remaining -= value;
            if (remaining < 0) {
                break;
            }
        }
        if (chosen == nullptr) {
            return refuse(context, "empty_pool");
        }
        if (chosen->supplementalIndex != definitions::kAbsent && !chosen->supplementalMissing) {
            return refuse(context, "supplemental_reward");
        }
        // A missing supplemental definition does not cancel the selected item grant.
        if (chosen->poolIndex != definitions::kAbsent) {
            // A pool reference selects one leaf; quantity belongs to that leaf.
            double childTotal = 0;
            if (!pool_weight(chosen->poolIndex, category, depth + 1, childTotal)) {
                return false;
            }
            if (childTotal == 0) {
                return refuse(context, "empty_pool");
            }
            return draw(chosen->poolIndex, category, depth + 1, childTotal);
        }
        if (selectedCount == selectedEntries.size()) {
            failure = Resolution::exceeded;
            return refuse(context, "reward_selection_capacity");
        }
        selectedEntries[selectedCount] = chosen;
        selectedCategories[selectedCount++] = category;
        if (chosen->itemIndex == definitions::kAbsent) {
            leafDrawn = true;
            return true;
        }
        return acquire_item(*chosen);
    }

    /**
     * Completes one acquisition after its children have finished.
     * @param wrapper True for a consumed wrapper, whose flag the grant sets without a row.
     * @return False when the flag lies outside the account bank.
     */
    bool acquire_flag(const definitions::Item& item, bool wrapper) noexcept {
        if (item.acquiredFlag == definitions::kAbsent) {
            return true;
        }
        if (item.acquiredFlag >= acquiredFlags.size()) {
            return refuse(context, "acquisition_flag");
        }
        acquiredFlags.set(item.acquiredFlag);
        if (wrapper) {
            result.wrapperFlags.set(item.acquiredFlag);
        }
        return true;
    }

    /**
     * Acquires a selected item, opening automatic wrappers before the next parent draw.
     * @param chosen Drawn entry that names the item.
     * @return False when the entry is malformed or the grant rows are full.
     */
    bool acquire_item(const definitions::Entry& chosen) noexcept {
        if (chosen.quantity == 0 || chosen.quantity > kMaximumQuantity) {
            return refuse(context, "reward_quantity");
        }
        if (!definitions::fits(chosen.sockets, data.sockets)) {
            return refuse(context, "socket_range");
        }
        const auto& item = data.items[chosen.itemIndex];
        if (item.definitionHash == 0) {
            return refuse(context, "item_unavailable");
        }
        if (item.poolIndex != definitions::kAbsent
            && (item.flags & definitions::kOpenOnAcquisition) != 0) {
            if (chosen.quantity != 1) {
                return refuse(context, "wrapper_quantity");
            }
            // These sockets belong to the consumed instance, not its descendant items.
            if (chosen.sockets.count != 0) {
                return refuse(context, "wrapper_socket_override");
            }
            bool childLeafDrawn = false;
            Resolver child{data,
                           context,
                           result,
                           next_random(wrapperSeed),
                           wrapperSeed,
                           acquiredFlags,
                           excludedByUnlocks,
                           childLeafDrawn,
                           leafFlagAcquired,
                           failure,
                           wrapperDepth + 1};
            if (!child.open_wrapper(item)) {
                return false;
            }
            // A child's flag is set after opening whatever it drew, but only a child that drew
            // a leaf row makes the resolution non-empty.
            if (childLeafDrawn) {
                leafDrawn = true;
                leafFlagAcquired |= item.acquiredFlag != definitions::kAbsent;
            }
            return acquire_flag(item, true);
        }
        if (result.count == result.grants.size()) {
            failure = Resolution::exceeded;
            return refuse(context, "grant_capacity");
        }
        leafDrawn = true;
        auto& grant = result.grants[result.count++];
        grant.itemIndex = chosen.itemIndex;
        grant.quantity = static_cast<std::int32_t>(chosen.quantity);
        if (chosen.sockets.count > grant.sockets.size()) {
            return refuse(context, "socket_capacity");
        }
        grant.socketCount = chosen.sockets.count;
        std::copy_n(
            data.sockets.begin() + chosen.sockets.first, grant.socketCount, grant.sockets.begin());
        return acquire_flag(item, false);
    }

    /**
     * Draws every category selection a wrapper declares from its pool.
     * A selection stops early once completed acquisitions leave nothing to draw.
     * @return False when the wrapper is too deep, malformed, or a draw is refused.
     */
    bool open_wrapper(const definitions::Item& item) noexcept {
        if (wrapperDepth >= definitions::kTraversalDepth) {
            return refuse(context, "wrapper_depth");
        }
        if (item.selectionCount == 0 || item.selectionCount > item.selections.size()) {
            return refuse(context, "wrapper_selections");
        }
        for (std::size_t i = 0; i < item.selectionCount; ++i) {
            const auto& declared = item.selections[i];
            for (std::size_t j = 0; j < declared.count; ++j) {
                double total = 0;
                if (!pool_weight(item.poolIndex, declared.categoryHash, 0, total)) {
                    return false;
                }
                // Completed acquisitions can leave fewer eligible members than requested.
                if (total == 0) {
                    break;
                }
                if (!draw(item.poolIndex, declared.categoryHash, 0, total)) {
                    return false;
                }
            }
        }
        return true;
    }
};

/** Acquiring an item grants it whole; opening draws a held stored wrapper's contents. */
enum class Plan : std::uint8_t { acquire, open };

/**
 * Resolves one item against the published graph: a stored item grants itself, and an automatic
 * wrapper opens into its draws and its own acquisition flag. Opening a stored wrapper draws the
 * same way; its flag was set when it was acquired.
 * @param result Receives the grants; cleared and left empty on failure.
 * @param failure Receives ineligible, exceeded or refused when the resolution fails.
 * @return False when nothing could be granted.
 */
bool resolve_item(definitions::View data,
                  const Context& context,
                  std::uint16_t itemIndex,
                  std::uint32_t quantity,
                  Plan plan,
                  Result& result,
                  Resolution& failure) noexcept {
    result = {};
    if (itemIndex >= data.items.size() || quantity == 0 || quantity > kMaximumQuantity) {
        return refuse(context, "reward_shape");
    }
    const auto& item = data.items[itemIndex];
    if (item.definitionHash == 0) {
        return refuse(context, "item_unavailable");
    }
    const bool wrapper = item.poolIndex != definitions::kAbsent;
    const bool automatic = wrapper && (item.flags & definitions::kOpenOnAcquisition) != 0;
    if (plan == Plan::open && (!wrapper || automatic)) {
        return refuse(context, "stored_wrapper");
    }
    Result staged{};
    // Stored engrams retain their wrapper until an opening transaction.
    if (plan == Plan::acquire && !automatic) {
        staged.grants[0].itemIndex = itemIndex;
        staged.grants[0].quantity = static_cast<std::int32_t>(quantity);
        staged.count = 1;
    } else {
        if (quantity != 1) {
            return refuse(context, "wrapper_quantity");
        }
        std::uint64_t wrapperSeed = context.seed;
        AcquiredFlags acquiredFlags{};
        bool excludedByUnlocks = false;
        bool leafDrawn = false;
        bool leafFlagAcquired = false;
        Resolver resolver{data,
                          context,
                          staged,
                          context.seed,
                          wrapperSeed,
                          acquiredFlags,
                          excludedByUnlocks,
                          leafDrawn,
                          leafFlagAcquired,
                          failure};
        if (!resolver.open_wrapper(item)) {
            return false;
        }
        if (staged.count == 0 && !leafFlagAcquired) {
            failure =
                excludedByUnlocks && !leafDrawn ? Resolution::ineligible : Resolution::refused;
            return refuse(context, "empty_reward");
        }
        if (automatic && !resolver.acquire_flag(item, true)) {
            return false;
        }
    }
    result = staged;
    if (context.refusal != nullptr) {
        *context.refusal = nullptr;
    }
    return true;
}

/**
 * Plans one item under the reward catalog's read lock.
 * @return Resolved, or why not; refused also covers an unpublished graph.
 */
Resolution plan_item(const Context& context,
                     std::uint16_t itemIndex,
                     std::uint32_t quantity,
                     Plan plan,
                     Result& result) noexcept {
    struct Request {
        const Context& context;
        std::uint16_t item;
        std::uint32_t quantity;
        Plan plan;
        Result& result;
        Resolution failure;
    } request{context, itemIndex, quantity, plan, result, Resolution::refused};
    result = {};
    if (context.refusal != nullptr) {
        *context.refusal = "reward_item";
    }
    if (build_data::read_reward_definitions(
            &request, [](void* raw, definitions::View data) noexcept {
                auto& value = *static_cast<Request*>(raw);
                return resolve_item(data,
                                    value.context,
                                    value.item,
                                    value.quantity,
                                    value.plan,
                                    value.result,
                                    value.failure);
            })) {
        return Resolution::resolved;
    }
    return request.failure;
}

} // namespace

bool eligible(std::span<const definitions::Instruction> instructions,
              const Context& context,
              bool& result) noexcept {
    return condition(instructions, context, result);
}

/** Shares the condition evaluator's checks, so an external read refuses the program. */
bool evaluate(std::span<const definitions::Instruction> program,
              const Context& context,
              std::int32_t& value) noexcept {
    value = 0;
    return evaluate_program(program, context, value);
}

/**
 * Plans an acquisition without changing State, under the reward catalog's read lock.
 * @param result Receives the grants and consumed wrapper flags.
 * @return Resolved, or why not; refused also covers an unpublished graph.
 */
Resolution resolve(const Context& context,
                   std::uint16_t itemIndex,
                   std::uint32_t quantity,
                   Result& result) noexcept {
    return plan_item(context, itemIndex, quantity, Plan::acquire, result);
}

/** Draws once, as an automatic wrapper does when it is acquired. */
Resolution open(const Context& context, std::uint16_t itemIndex, Result& result) noexcept {
    return plan_item(context, itemIndex, 1, Plan::open, result);
}

} // namespace sunrise::state::reward_resolution
