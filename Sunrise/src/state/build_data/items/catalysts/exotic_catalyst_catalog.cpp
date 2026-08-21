#include "exotic_catalyst_catalog.h"

#include <algorithm>
#include <atomic>

#include "../../../account/inventory/item_state.h"
#include "../../../unlocks/definition.h"
#include "../../table.h"
#include "../details/definition.h"

namespace sunrise::state::build_data::items::catalysts {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
std::atomic<bool> g_completionEnabled{true};

/**
 * Finds one item in a sorted catalog while its lock is held.
 * @param definitions Catalyst definitions in native item index order.
 * @param itemDefinitionIndex Native item index to find.
 * @return The matching definition, or null when no definition matches.
 */
[[nodiscard]] const Definition* find(std::span<const Definition> definitions,
                                     std::uint16_t itemDefinitionIndex) noexcept {
    const auto found = std::lower_bound(
        definitions.begin(),
        definitions.end(),
        itemDefinitionIndex,
        [](const Definition& value, auto key) { return value.itemDefinitionIndex < key; });
    return found != definitions.end() && found->itemDefinitionIndex == itemDefinitionIndex
               ? &*found
               : nullptr;
}

/** Sets one flag slot and removes duplicate authored rows for that slot. */
[[nodiscard]] bool upsert_flag(state::Family5State& family, std::uint16_t slot) noexcept {
    const std::size_t oldCount = family.flagCount;
    std::size_t write = 0;
    bool found = false;
    for (std::size_t read = 0; read < oldCount; ++read) {
        const state::UnlockFlagOverride row = family.flags[read];
        if (row.slot == slot) {
            if (!found) {
                family.flags[write++] = {slot, state::unlocks::kFlagSet};
                found = true;
            }
            continue;
        }
        family.flags[write++] = row;
    }
    if (!found) {
        if (write >= family.flags.size()) {
            return false;
        }
        family.flags[write++] = {slot, state::unlocks::kFlagSet};
    }
    for (std::size_t index = write; index < oldCount; ++index) {
        family.flags[index] = {};
    }
    family.flagCount = write;
    return true;
}

/** Raises one signed value slot and removes duplicate authored rows for that slot. */
[[nodiscard]] bool upsert_completion_value(state::Family5State& family,
                                           std::uint16_t slot,
                                           std::int32_t minimum) noexcept {
    const std::size_t oldCount = family.valueCount;
    std::size_t write = 0;
    std::optional<std::size_t> mergedIndex;
    for (std::size_t read = 0; read < oldCount; ++read) {
        const state::UnlockValueOverride row = family.values[read];
        if (row.slot == slot) {
            if (!mergedIndex.has_value()) {
                mergedIndex = write;
                family.values[write++] = {slot, (std::max)(row.value, minimum)};
            } else {
                state::UnlockValueOverride& merged = family.values[*mergedIndex];
                merged.value = (std::max)(merged.value, row.value);
            }
            continue;
        }
        family.values[write++] = row;
    }
    if (!mergedIndex.has_value()) {
        if (write >= family.values.size()) {
            return false;
        }
        family.values[write++] = {slot, minimum};
    }
    for (std::size_t index = write; index < oldCount; ++index) {
        family.values[index] = {};
    }
    family.valueCount = write;
    return true;
}

/** @return True when one requirement set uses a strict prefix and a zero tail. */
[[nodiscard]] bool valid_completion(const CompletionRequirements& completion) noexcept {
    if (completion.flagCount > completion.flags.size()
        || completion.valueCount > completion.values.size()
        || (completion.flagCount == 0 && completion.valueCount == 0)) {
        return false;
    }
    for (std::size_t index = 0; index < completion.flags.size(); ++index) {
        if (index < completion.flagCount) {
            if (completion.flags[index] >= kUnavailableCompletionFlagIndex
                || (index != 0 && completion.flags[index - 1] >= completion.flags[index])) {
                return false;
            }
        } else if (completion.flags[index] != 0) {
            return false;
        }
    }
    for (std::size_t index = 0; index < completion.values.size(); ++index) {
        const CompletionValue& value = completion.values[index];
        if (index < completion.valueCount) {
            if (value.index >= kUnavailableCompletionValueIndex || value.minimum <= 0
                || (index != 0 && completion.values[index - 1].index >= value.index)) {
                return false;
            }
        } else if (value.index != 0 || value.minimum != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

void set_completion_enabled(bool enabled) noexcept {
    g_completionEnabled.store(enabled, std::memory_order_release);
}

bool completion_enabled() noexcept {
    return g_completionEnabled.load(std::memory_order_acquire);
}

bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const Definition& definition = definitions[index];
        const bool hasCompletedPlug =
            definition.completedPlugDefinitionIndex != details::kUnavailableItemIndex;
        const bool hasEffect = definition.effectDefinitionIndex != details::kUnavailableItemIndex;
        const bool hasProgress =
            definition.progressPlugDefinitionIndex != details::kUnavailableItemIndex;
        const bool hasAcquisition =
            definition.acquisitionDefinitionIndex != kUnavailableAcquisitionIndex;
        const bool hasObjective =
            definition.objective.definitionIndex != kUnavailableObjectiveIndex;
        const bool directEffect =
            definition.completedPlugDefinitionIndex == definition.effectDefinitionIndex;
        if (definition.itemDefinitionHash == 0
            || definition.socketLane >= details::kInitialPlugCapacity
            || !valid_availability(definition.availability)
            || (definition.availability == Availability::unsupported
                && (hasCompletedPlug || hasProgress || hasEffect || hasAcquisition
                    || definition.completion.flagCount != 0
                    || definition.completion.valueCount != 0 || hasObjective
                    || definition.objective.value != 0))
            || (definition.availability != Availability::unsupported
                && (!hasCompletedPlug || !hasEffect || !hasAcquisition
                    || !valid_completion(definition.completion)))
            || (hasObjective != (definition.objective.value > 0))
            || (definition.availability != Availability::unsupported
                && ((directEffect && (hasProgress || hasObjective))
                    || (!directEffect && !hasProgress)))
            || (index != 0 && !definition_index_less(definitions[index - 1], definition))) {
            return false;
        }
    }
    return true;
}

bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_definitions.replace(definitions);
}

Result resolve(std::uint16_t itemDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    const Definition* definition = find(g_definitions.rows(), itemDefinitionIndex);
    if (definition == nullptr) {
        return {};
    }
    if (definition->availability == Availability::placeholder) {
        return {Error::placeholderOnly, Availability::placeholder, {}};
    }
    if (definition->availability != Availability::released) {
        return {Error::ambiguousLifecycle, Availability::unsupported, {}};
    }
    return {Error::none,
            Availability::released,
            {definition->socketLane,
             definition->completedPlugDefinitionIndex,
             definition->progressPlugDefinitionIndex,
             definition->effectDefinitionIndex,
             definition->acquisitionDefinitionIndex,
             definition->completion,
             definition->objective}};
}

std::uint16_t resolve_effect(std::uint16_t itemDefinitionIndex,
                             std::uint8_t socketLane,
                             std::uint16_t plugDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    const Definition* definition = find(g_definitions.rows(), itemDefinitionIndex);
    if (definition == nullptr || definition->availability != Availability::released
        || definition->socketLane != socketLane
        || (definition->completedPlugDefinitionIndex != plugDefinitionIndex
            && definition->effectDefinitionIndex != plugDefinitionIndex)
        || definition->effectDefinitionIndex == details::kUnavailableItemIndex) {
        return plugDefinitionIndex;
    }
    return definition->effectDefinitionIndex;
}

bool owns_lane(std::uint16_t itemDefinitionIndex, std::uint8_t socketLane) noexcept {
    const Lock::Shared guard(g_lock);
    const Definition* definition = find(g_definitions.rows(), itemDefinitionIndex);
    return definition != nullptr && definition->socketLane == socketLane;
}

ApplyResult apply_completed(std::uint16_t itemDefinitionIndex,
                            std::uint32_t& flags,
                            std::span<std::optional<std::uint16_t>> plugs) noexcept {
    if (!completion_enabled()) {
        return ApplyResult::unchanged;
    }
    const Result result = resolve(itemDefinitionIndex);
    if (result.error == Error::noCatalyst || result.availability != Availability::released) {
        return ApplyResult::unchanged;
    }
    if (result.error != Error::none || !account::inventory::valid_item_state(flags)
        || result.completed.socketLane >= plugs.size()
        || result.completed.effectDefinitionIndex == details::kUnavailableItemIndex) {
        return ApplyResult::failed;
    }

    const std::uint32_t completedFlags = flags | account::inventory::kMasterworkItemFlag;
    const std::optional<std::uint16_t> completedPlug = result.completed.effectDefinitionIndex;
    plugs[result.completed.socketLane] = completedPlug;
    flags = completedFlags;
    return ApplyResult::completed;
}

bool append_investment_overrides(state::Family5State& family) noexcept {
    if (!completion_enabled()) {
        return true;
    }
    if (family.flagCount > family.flags.size() || family.valueCount > family.values.size()) {
        return false;
    }

    state::Family5State candidate = family;
    const Lock::Shared guard(g_lock);
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.availability != Availability::released) {
            continue;
        }
        if (!upsert_flag(candidate, definition.acquisitionDefinitionIndex)) {
            return false;
        }
        for (std::size_t flag = 0; flag < definition.completion.flagCount; ++flag) {
            if (!upsert_flag(candidate, definition.completion.flags[flag])) {
                return false;
            }
        }
        for (std::size_t value = 0; value < definition.completion.valueCount; ++value) {
            const CompletionValue& requirement = definition.completion.values[value];
            if (!upsert_completion_value(candidate, requirement.index, requirement.minimum)) {
                return false;
            }
        }
    }
    family = candidate;
    return true;
}

bool append_objective_completions(std::span<std::int32_t> values) noexcept {
    if (!completion_enabled()) {
        return true;
    }
    const Lock::Shared guard(g_lock);
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.availability == Availability::released
            && definition.objective.definitionIndex != kUnavailableObjectiveIndex
            && (definition.objective.definitionIndex >= values.size()
                || definition.objective.value <= 0)) {
            return false;
        }
    }
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.availability == Availability::released
            && definition.objective.definitionIndex != kUnavailableObjectiveIndex) {
            std::int32_t& value = values[definition.objective.definitionIndex];
            value = (std::max)(value, definition.objective.value);
        }
    }
    return true;
}

bool snapshot(std::span<Definition> output, std::size_t& outputCount) noexcept {
    outputCount = 0;
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, outputCount);
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::items::catalysts
