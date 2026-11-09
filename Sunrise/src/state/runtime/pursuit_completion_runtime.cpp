#include "pursuit_completion_runtime.h"

#include <algorithm>
#include <cstddef>

#include "../build_data/runtime.h"
#include "../rewards/reward_resolver.h"

namespace sunrise::state {
namespace {

namespace objectives = build_data::objectives;

/**
 * Reads the per-item value a slot names from one instance's values.
 * @return False when the instance's contract does not carry the slot.
 */
[[nodiscard]] bool read_item_value(std::uint16_t slot,
                                   const build_data::pursuits::Definition& definition,
                                   std::span<const std::int32_t> itemValues,
                                   std::int32_t& value) noexcept {
    const auto slots = std::span(definition.itemValueSlots).first(definition.itemValueSlotCount);
    const auto found = std::find(slots.begin(), slots.end(), slot);
    const auto position = static_cast<std::size_t>(found - slots.begin());
    if (found == slots.end() || position >= itemValues.size()) {
        return false;
    }
    value = itemValues[position];
    return true;
}

/**
 * Raises a value to the largest one a slot holds across the held pursuits that carry it. An
 * instance with no values yet holds zero in every lane.
 * @param found Whether value already holds a read of the slot.
 * @return True when value holds a read of the slot.
 */
[[nodiscard]] bool read_held_maximum(std::uint16_t slot,
                                     std::span<const account::inventory::Item> heldItems,
                                     bool found,
                                     std::int32_t& value) noexcept {
    for (const account::inventory::Item& held : heldItems) {
        build_data::items::Definition item{};
        build_data::pursuits::Definition contract{};
        const account::inventory::ItemObjectives values =
            held.objectives.value_or(account::inventory::ItemObjectives{});
        std::int32_t carried = 0;
        if (!build_data::find_item_definition_hash(held.definitionHash, item)
            || !build_data::items::pursuit_bucket_item(item)
            || !build_data::find_pursuit_definition(item.definitionIndex, contract)
            || !read_item_value(slot, contract, values, carried)) {
            continue;
        }
        if (!found || carried > value) {
            value = carried;
        }
        found = true;
    }
    return found;
}

/**
 * Reads an objective's progress: the per-item value its slot names when the held item carries it,
 * or that slot's largest value across held instances when the objective reads that, otherwise its
 * program evaluated against the saved banks.
 * @return False when the value cannot be read.
 */
[[nodiscard]] bool read_progress(const objectives::Definition& objective,
                                 const build_data::pursuits::Definition& definition,
                                 std::span<const std::int32_t> itemValues,
                                 std::span<const account::inventory::Item> heldItems,
                                 const reward_resolution::Context& context,
                                 std::int32_t& value) noexcept {
    if (objective.itemValueSlot == objectives::kNoItemValueSlot) {
        return reward_resolution::evaluate(objective.expression, context, value);
    }
    const bool found = read_item_value(objective.itemValueSlot, definition, itemValues, value);
    if (objective.itemValueMode != objectives::ItemValueMode::maximum) {
        return found;
    }
    return read_held_maximum(objective.itemValueSlot, heldItems, found, value);
}

/** Evaluates one objective's progress and adjusted target with its installed comparison. */
[[nodiscard]] PursuitObjectiveStatus
inspect_pursuit_objective(const objectives::Definition& objective,
                          const build_data::pursuits::Definition& definition,
                          std::span<const std::int32_t> itemValues,
                          std::span<const account::inventory::Item> heldItems,
                          const reward_resolution::Context& context) noexcept {
    PursuitObjectiveStatus status{};
    status.hash = objective.definitionHash;
    status.target = objective.completionValue;
    status.itemHeld = objective.itemValueSlot != objectives::kNoItemValueSlot;
    std::int32_t adjustment = 0;
    if (!objective.readable || !objective.adjustmentReadable
        || (!objective.targetAdjustment.empty()
            && !reward_resolution::evaluate(objective.targetAdjustment, context, adjustment))
        || !read_progress(objective, definition, itemValues, heldItems, context, status.value)) {
        return status;
    }
    // The native signed subtraction wraps at the 32-bit boundary.
    status.target = static_cast<std::int32_t>(static_cast<std::uint32_t>(status.target)
                                              - static_cast<std::uint32_t>(adjustment));
    status.known = true;
    status.complete =
        objective.optional
        || (objective.countsDown ? status.value != 0 && status.value <= status.target
                                 : status.target != 0 && status.value >= status.target);
    return status;
}

} // namespace

/** An unknown objective still leaves the aggregate determinate when a known one settles it. */
PursuitCompletionStatus
inspect_pursuit_completion(const build_data::pursuits::Definition& definition,
                           const reward_resolution::Context& context,
                           std::span<const std::int32_t> itemValues,
                           std::span<const account::inventory::Item> heldItems) noexcept {
    PursuitCompletionStatus status{};
    if (!definition.readable) {
        return status;
    }
    status.objectiveCount = definition.objectiveCount;
    std::size_t completed = 0;
    std::size_t incomplete = 0;
    std::size_t unknown = 0;
    for (std::size_t i = 0; i < definition.objectiveCount; ++i) {
        PursuitObjectiveStatus& result = status.objectives[i];
        objectives::Definition objective{};
        if (!build_data::find_objective_definition(definition.objectives[i], objective)) {
            ++unknown;
            continue;
        }
        if (definition.requireNonOptionalCompletion && objective.optional) {
            result.hash = objective.definitionHash;
            result.known = true;
            result.skipped = true;
            result.target = objective.completionValue;
            continue;
        }
        result = inspect_pursuit_objective(objective, definition, itemValues, heldItems, context);
        if (!result.known) {
            ++unknown;
        } else if (result.complete) {
            ++completed;
        } else {
            ++incomplete;
        }
    }
    status.objectivesKnown =
        unknown == 0 || (definition.requireAllObjectives ? incomplete != 0 : completed != 0);
    status.objectivesComplete =
        definition.requireAllObjectives
            ? unknown == 0 && incomplete == 0
                  && (!definition.requireNonOptionalCompletion || completed != 0)
            : completed != 0;
    return status;
}

} // namespace sunrise::state
