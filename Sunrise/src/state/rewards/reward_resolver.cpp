#include "reward_resolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../build_data/runtime.h"
#include "../unlocks/unlocks_expression.h"

namespace sunrise::state::rewards {
namespace {

namespace definitions = build_data::rewards;
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

/** Reads one validated flag from the caller's unlock banks or selected character. */
bool read_flag(const void* raw, const unlocks::Instruction& instruction, bool& set) noexcept {
    const auto& context = *static_cast<const Context*>(raw);
    const auto index = instruction.operand;
    switch (instruction.bank) {
    case unlocks::Bank::account:
        set = context.unlocks.accountFlags[index] == unlocks::kFlagSet;
        return true;
    case unlocks::Bank::profile:
        set = context.unlocks.profileFlags[index] == unlocks::kFlagSet;
        return true;
    case unlocks::Bank::character:
        set = context.unlocks.characterObjectFlags[index] == unlocks::kFlagSet;
        return true;
    case unlocks::Bank::characterClass:
        set = index == static_cast<std::uint32_t>(context.characterClass);
        return true;
    default:
        return false;
    }
}

/** Reads one validated value from the caller's unlock banks. */
bool read_value(const void* raw,
                const unlocks::Instruction& instruction,
                std::int32_t& value) noexcept {
    const auto& context = *static_cast<const Context*>(raw);
    switch (instruction.bank) {
    case unlocks::Bank::account:
        value = context.unlocks.objectiveValues[instruction.operand];
        return true;
    case unlocks::Bank::character:
        value = context.unlocks.characterObjectValues[instruction.operand];
        return true;
    default:
        return false;
    }
}

/** An empty program is unconditional; external identities are not in the saved banks. */
bool condition(std::span<const definitions::Instruction> program,
               const Context& context,
               bool& result) noexcept {
    result = program.empty();
    if (program.empty()) {
        return true;
    }
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
    const unlocks::Inputs inputs{read_flag, read_value, &context};
    return unlocks::evaluate(program, inputs, result) || refuse(context, "condition_shape");
}

bool condition(definitions::View data,
               definitions::Range expression,
               const Context& context,
               bool& result) noexcept {
    if (!definitions::fits(expression, data.instructions)) {
        result = false;
        return refuse(context, "condition_shape");
    }
    return condition(
        data.instructions.subspan(expression.first, expression.count), context, result);
}

struct Resolver {
    definitions::View data;
    const Context& context;
    Result& result;
    std::uint64_t random;
    std::array<const definitions::Entry*, kDrawCapacity> selectedEntries{};
    std::array<std::uint32_t, kDrawCapacity> selectedCategories{};
    std::size_t selectedCount{};
    bool excludedByUnlocks{};

    double fraction() noexcept {
        // SplitMix64 makes a prepared seed replayable without shared random state.
        random += 0x9E3779B97F4A7C15ULL;
        auto bits = random;
        bits = (bits ^ (bits >> 30)) * 0xBF58476D1CE4E5B9ULL;
        bits = (bits ^ (bits >> 27)) * 0x94D049BB133111EBULL;
        bits ^= bits >> 31;
        return static_cast<double>(bits >> 11) * 0x1.0p-53;
    }

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
        if (!condition(data, entry.condition, context, enabled)) {
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
            if (!condition(data, modifier.condition, context, enabled)) {
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
            return refuse(context, "reward_selection_capacity");
        }
        selectedEntries[selectedCount] = chosen;
        selectedCategories[selectedCount++] = category;
        if (chosen->itemIndex == definitions::kAbsent) {
            return true;
        }
        return append_grant(*chosen);
    }

    /** Appends the chosen leaf's item grant with the socket overrides its row carries. */
    bool append_grant(const definitions::Entry& chosen) noexcept {
        if (chosen.quantity == 0 || chosen.quantity > kMaximumQuantity) {
            return refuse(context, "reward_quantity");
        }
        if (result.count == result.grants.size()) {
            return refuse(context, "grant_capacity");
        }
        if (!definitions::fits(chosen.sockets, data.sockets)) {
            return refuse(context, "socket_range");
        }
        auto& grant = result.grants[result.count++];
        grant.itemIndex = chosen.itemIndex;
        grant.quantity = static_cast<std::int32_t>(chosen.quantity);
        if (chosen.sockets.count > grant.sockets.size()) {
            return refuse(context, "socket_capacity");
        }
        grant.socketCount = chosen.sockets.count;
        std::copy_n(
            data.sockets.begin() + chosen.sockets.first, grant.socketCount, grant.sockets.begin());
        return true;
    }
};

bool resolve_item(definitions::View data,
                  const Context& context,
                  std::uint16_t itemIndex,
                  std::uint32_t quantity,
                  Result& result,
                  bool& ineligible) noexcept {
    result = {};
    if (itemIndex >= data.items.size() || quantity == 0 || quantity > kMaximumQuantity) {
        return refuse(context, "reward_shape");
    }
    const auto& item = data.items[itemIndex];
    if (item.definitionHash == 0) {
        return refuse(context, "item_unavailable");
    }
    Result staged{};
    // Stored engrams retain their wrapper until an opening transaction.
    if (item.poolIndex == definitions::kAbsent
        || (item.flags & definitions::kOpenOnAcquisition) == 0) {
        staged.grants[0].itemIndex = itemIndex;
        staged.grants[0].quantity = static_cast<std::int32_t>(quantity);
        staged.count = 1;
    } else {
        if (quantity != 1) {
            return refuse(context, "wrapper_quantity");
        }
        if (item.selectionCount == 0 || item.selectionCount > item.selections.size()) {
            return refuse(context, "wrapper_selections");
        }
        Resolver resolver{data, context, staged, context.seed};
        for (std::size_t i = 0; i < item.selectionCount; ++i) {
            const auto& declared = item.selections[i];
            for (std::size_t j = 0; j < declared.count; ++j) {
                double total = 0;
                if (!resolver.pool_weight(item.poolIndex, declared.categoryHash, 0, total)) {
                    return false;
                }
                // A fixed bundle can have fewer eligible members after an acquisition unlock.
                if (total == 0) {
                    break;
                }
                if (!resolver.draw(item.poolIndex, declared.categoryHash, 0, total)) {
                    return false;
                }
            }
        }
        if (staged.count == 0) {
            ineligible = resolver.excludedByUnlocks;
            return refuse(context, "empty_reward");
        }
    }
    result = staged;
    if (context.refusal != nullptr) {
        *context.refusal = nullptr;
    }
    return true;
}

} // namespace

bool eligible(std::span<const definitions::Instruction> instructions,
              const Context& context,
              bool& result) noexcept {
    return condition(instructions, context, result);
}

Resolution resolve(const Context& context,
                   std::uint16_t itemIndex,
                   std::uint32_t quantity,
                   Result& result) noexcept {
    struct Request {
        const Context& context;
        std::uint16_t item;
        std::uint32_t quantity;
        Result& result;
        bool ineligible;
    } request{context, itemIndex, quantity, result, false};
    result = {};
    if (context.refusal != nullptr) {
        *context.refusal = "reward_item";
    }
    if (build_data::read_reward_definitions(&request,
                                            [](void* raw, definitions::View data) noexcept {
                                                auto& value = *static_cast<Request*>(raw);
                                                return resolve_item(data,
                                                                    value.context,
                                                                    value.item,
                                                                    value.quantity,
                                                                    value.result,
                                                                    value.ineligible);
                                            })) {
        return Resolution::resolved;
    }
    return request.ineligible ? Resolution::ineligible : Resolution::refused;
}

} // namespace sunrise::state::rewards
