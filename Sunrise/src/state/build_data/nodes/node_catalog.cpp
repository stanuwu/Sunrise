#include "node_catalog.h"
#include "../../../core/logging/log.h"
#include <cstdio>
#include <array>

#include "../../unlocks/definition.h"
#include "../table.h"

namespace sunrise::state::build_data::nodes {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;

} // namespace

/** Clears every generated node definition under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

/** Checks that the definitions are dense and in native index order. */
bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        if (definitions[row].definitionIndex != row
            || definitions[row].childCount > kChildCapacity) {
            return false;
        }
    }
    return true;
}

/** Replaces the generated node definitions in one step. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    return g_definitions.replace(definitions);
}

/** Runs one callable over every node that drives a value slot. */
void for_each_driving(void* context,
                      void (*visit)(void* context, const Definition& definition)) noexcept {
    if (visit == nullptr) {
        return;
    }
    const Lock::Shared guard(g_lock);
    for (const Definition& definition : g_definitions.rows()) {
        if (definition.childCount != 0 && definition.valueIndex != kUnavailableValueIndex) {
            visit(context, definition);
        }
    }
}

/** Copies every row in native node order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return Number of generated node definitions, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

/** Calls back for every node, under the shared lock. */
void for_each(void* context, void (*visit)(void*, const Definition&) noexcept) noexcept {
    const Lock::Shared guard(g_lock);
    for (const Definition& node : g_definitions.rows()) {
        visit(context, node);
    }
}

/** Sets the visibility gate of every lore book category. */
std::size_t apply_visibility(std::span<std::uint8_t> accountFlags) noexcept {
    const Lock::Shared guard(g_lock);
    std::size_t set = 0;
    for (const Definition& node : g_definitions.rows()) {
        if (node.definitionIndex < kLoreNodeFirst || node.definitionIndex > kLoreNodeLast
            || node.visibilityFlagIndex == kUnavailableFlagIndex
            || static_cast<std::size_t>(node.visibilityFlagIndex) >= accountFlags.size()) {
            continue;
        }
        accountFlags[node.visibilityFlagIndex] = unlocks::kFlagSet;
        ++set;
    }
    return set;
}

/** Sets the character scoped visibility gates of the lore book categories. */
std::size_t apply_character_visibility(std::span<std::byte> characterFlags) noexcept {
    const Lock::Shared guard(g_lock);
    std::size_t set = 0;
    for (const Definition& node : g_definitions.rows()) {
        if (node.definitionIndex < kLoreNodeFirst || node.definitionIndex > kLoreNodeLast
            || node.visibilityCharacterFlagIndex == kUnavailableFlagIndex
            || static_cast<std::size_t>(node.visibilityCharacterFlagIndex)
                   >= characterFlags.size()) {
            continue;
        }
        characterFlags[node.visibilityCharacterFlagIndex] =
            static_cast<std::byte>(unlocks::kFlagSet);
        ++set;
    }
    return set;
}

} // namespace sunrise::state::build_data::nodes
