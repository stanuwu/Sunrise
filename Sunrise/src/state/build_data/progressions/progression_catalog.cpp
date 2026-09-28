#include "progression_catalog.h"

#include <algorithm>
#include <shared_mutex>

#include "../table.h"
#include "core/threading/srw_lock.h"

namespace sunrise::state::build_data::progressions {
namespace {

// One lock covers both tables. A definition names its steps by range, so a reader must never
// see one table replaced and the other not.
core::threading::SrwLock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
Table<Step, kStepCapacity> g_steps;

} // namespace

/** Clears every generated progression definition and its step bank under the catalog lock. */
void clear() noexcept {
    const std::lock_guard guard(g_lock);
    g_definitions.clear();
    g_steps.clear();
}

/** Checks that the definitions are dense, in native index order, and own the whole step bank. */
bool valid(std::span<const Definition> definitions, std::span<const Step> steps) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity
        || steps.size() > kStepCapacity) {
        return false;
    }
    std::size_t stepOffset = 0;
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        const Definition& definition = definitions[row];
        if (definition.definitionIndex != row || definition.stepOffset != stepOffset
            || definition.stepCount > steps.size() - stepOffset) {
            return false;
        }
        stepOffset += definition.stepCount;
    }
    return stepOffset == steps.size();
}

/** Replaces the generated progression definitions and their step bank in one step. */
bool replace(std::span<const Definition> definitions, std::span<const Step> steps) noexcept {
    if (!valid(definitions, steps)) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    const bool replaced = g_definitions.replace(definitions) && g_steps.replace(steps);
    if (!replaced) {
        g_definitions.clear();
        g_steps.clear();
    }
    return replaced;
}

/**
 * Reads one definition under the catalog lock.
 * @param definitionIndex Native progression definition index.
 * @param definition Receives the row, or an empty row when absent.
 * @return False when the catalog does not hold the index.
 */
bool find(std::uint16_t definitionIndex, Definition& definition) noexcept {
    definition = {};
    const std::shared_lock guard(g_lock);
    const auto rows = g_definitions.rows();
    if (definitionIndex >= rows.size()) {
        return false;
    }
    definition = rows[definitionIndex];
    return true;
}

/** Copies the rank steps one progression declares, in rank order. */
bool steps(std::uint16_t definitionIndex, std::span<Step> output, std::size_t& count) noexcept {
    count = 0;
    const std::shared_lock guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    if (definitionIndex >= rows.size()) {
        return false;
    }
    const Definition& definition = rows[definitionIndex];
    const std::span<const Step> bank = g_steps.rows();
    if (output.size() < definition.stepCount || definition.stepOffset > bank.size()
        || definition.stepCount > bank.size() - definition.stepOffset) {
        return false;
    }
    std::copy_n(bank.begin() + definition.stepOffset, definition.stepCount, output.begin());
    count = definition.stepCount;
    return true;
}

/** Copies the whole flat step bank. */
bool snapshot_steps(std::span<Step> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_steps.snapshot(output, count);
}

/** @return The step bank row count, read under the lock. */
std::size_t step_count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_steps.count();
}

/** Lists the definition index each slot of one scope's record array carries. */
bool slots(Scope scope, std::span<std::uint16_t> output, std::size_t& count) noexcept {
    count = 0;
    const std::shared_lock guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    bool complete = !rows.empty();
    for (const Definition& row : rows) {
        if (!complete) {
            break;
        }
        if (row.scope != scope) {
            continue;
        }
        // A scope with more definitions than its array has slots would lose the tail in silence.
        complete = count < output.size();
        if (complete) {
            output[count++] = row.definitionIndex;
        }
    }
    if (!complete) {
        count = 0;
    }
    return complete;
}

/** Copies every row in native definition order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return Number of generated progression definitions, read under the lock. */
std::size_t count() noexcept {
    const std::shared_lock guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::progressions
