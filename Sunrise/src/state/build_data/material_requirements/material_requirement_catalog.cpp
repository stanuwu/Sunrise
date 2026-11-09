#include "material_requirement_catalog.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <shared_mutex>

#include "../../unlocks/definition.h"
#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::material_requirements {
namespace {

core::threading::SrwLock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
Table<CurrencyScalar, kCurrencyScalarCapacity> g_scalars;

/** Orders scalars by socket type, then currency, so a lookup is a binary search. */
[[nodiscard]] bool precedes(const CurrencyScalar& left, const CurrencyScalar& right) noexcept {
    return left.socketType != right.socketType
               ? left.socketType < right.socketType
               : left.itemDefinitionIndex < right.itemDefinitionIndex;
}

/** @return True when every scalar names a currency and multiplies, in strictly sorted order. */
[[nodiscard]] bool valid_scalars(std::span<const CurrencyScalar> scalars) noexcept {
    if (scalars.size() > kCurrencyScalarCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < scalars.size(); ++index) {
        if (scalars[index].itemDefinitionIndex == kUnavailableItemDefinitionIndex
            || scalars[index].multiplier == 0
            || (index != 0 && !precedes(scalars[index - 1], scalars[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace

void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_definitions.clear();
    g_scalars.clear();
}

/**
 * Checks one complete requirement-set table and its socket scalars before they are published.
 * A set owns its index, so a duplicate or out-of-range index rejects the whole table.
 * @param definitions Candidate rows, indexed by requirement-set index.
 * @param scalars Candidate socket-type currency scalars; a build may declare none.
 * @return True when every set is unique and in range, its unused rows are clear, and the scalars
 * are sorted and complete.
 */
bool valid(std::span<const Definition> definitions,
           std::span<const CurrencyScalar> scalars) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity
        || !valid_scalars(scalars)) {
        return false;
    }
    std::array<bool, kDefinitionCapacity> occupied{};
    for (const Definition& definition : definitions) {
        if (definition.requirementSetHash == 0
            || definition.requirementSetIndex >= definitions.size()
            || occupied[definition.requirementSetIndex]
            || definition.requirementCount > definition.requirements.size()) {
            return false;
        }
        for (std::size_t index = 0; index < definition.requirements.size(); ++index) {
            const Requirement& requirement = definition.requirements[index];
            if (index < definition.requirementCount) {
                if (requirement.itemDefinitionIndex == kUnavailableItemDefinitionIndex
                    || (requirement.condition == kUnconditionalRequirement
                        && requirement.valueIndex != kUnavailableValueIndex)
                    || (requirement.valueIndex != kUnavailableValueIndex
                        && requirement.valueIndex >= unlocks::kObjectiveValueCapacity)) {
                    return false;
                }
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (definition.requirements[prior].itemDefinitionIndex
                        == requirement.itemDefinitionIndex) {
                        return false;
                    }
                }
            } else if (requirement.itemDefinitionIndex != kUnavailableItemDefinitionIndex
                       || requirement.quantity != 0
                       || requirement.condition != kUnconditionalRequirement
                       || requirement.valueIndex != kUnavailableValueIndex
                       || requirement.deleteOnAction || requirement.omitFromRequirements) {
                return false;
            }
        }
        occupied[definition.requirementSetIndex] = true;
    }
    return true;
}

/**
 * Validates one table and its scalars and publishes both, placing each set at its own index.
 * @param definitions Candidate rows, rejected as a whole when any row is invalid.
 * @param scalars Candidate socket-type currency scalars, published with the sets.
 * @return True when both replaced the published ones.
 */
bool replace(std::span<const Definition> definitions,
             std::span<const CurrencyScalar> scalars) noexcept {
    if (!valid(definitions, scalars)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    const std::span<Definition> storage = g_definitions.reset(definitions.size());
    // valid() bounded both counts, so neither store refuses and the pair cannot half publish.
    if (storage.size() != definitions.size() || !g_scalars.replace(scalars)) {
        return false;
    }
    for (const Definition& definition : definitions) {
        storage[definition.requirementSetIndex] = definition;
    }
    return true;
}

bool find(std::uint16_t requirementSetIndex, Definition& definition) noexcept {
    definition = {};
    const std::shared_lock guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    const bool found = static_cast<std::size_t>(requirementSetIndex) < rows.size();
    if (found) {
        definition = rows[requirementSetIndex];
    }
    return found;
}

std::uint32_t currency_multiplier(std::uint16_t socketType,
                                  std::uint16_t itemDefinitionIndex) noexcept {
    const CurrencyScalar key{socketType, itemDefinitionIndex, 0};
    const std::shared_lock guard(g_lock);
    const std::span<const CurrencyScalar> rows = g_scalars.rows();
    const auto found = std::lower_bound(rows.begin(), rows.end(), key, precedes);
    return found != rows.end() && found->socketType == socketType
                   && found->itemDefinitionIndex == itemDefinitionIndex
               ? found->multiplier
               : 1U;
}

bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.snapshot(output, count);
}

bool snapshot_currency_scalars(std::span<CurrencyScalar> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_scalars.snapshot(output, count);
}

std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::material_requirements
