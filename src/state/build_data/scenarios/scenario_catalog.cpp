#include "scenario_catalog.h"

#include "../table.h"

namespace sunrise::state::build_data::scenarios {
namespace {

// One lock covers both tables: a reader must never see new layouts against old roster groups.
Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
Table<RosterGroup, kRosterGroupCapacity> g_groups;

/** @param definition Candidate row. @return Its name as a bounded view. */
[[nodiscard]] std::string_view name_of(const Definition& definition) noexcept {
    return {definition.name.data(), definition.nameLength};
}

/** @param group Candidate roster group. @return True when every field is canonical. */
[[nodiscard]] bool canonical(const RosterGroup& group) noexcept {
    if (group.registryKey == 0 || group.slotCount == 0 || group.slotCount > kRosterSlotCapacity) {
        return false;
    }
    for (std::size_t slot = 0; slot < kRosterSlotCapacity; ++slot) {
        const bool declared = slot < group.slotCount;
        if (declared
            && (group.slotTypes[slot] == 0 || group.slotTypes[slot] > kMaximumSlotType
                || (group.slotFlags[slot] & ~kSlotFlagMask) != 0)) {
            return false;
        }
        // Storage past the declared count must stay zero, or two caches of the same packages
        // could differ byte for byte while meaning the same thing.
        if (!declared && (group.slotTypes[slot] != 0 || group.slotFlags[slot] != 0)) {
            return false;
        }
    }
    return true;
}

/**
 * @param definition Candidate row.
 * @param groupCount Published roster group count.
 * @return True when every field is canonical.
 */
[[nodiscard]] bool canonical(const Definition& definition, std::size_t groupCount) noexcept {
    if (definition.nameLength == 0 || definition.nameLength > kNameCapacity
        || definition.bubbleCount > kBubbleCapacity || definition.truncated > 1
        || definition.rosterGroupCount > kDestinationGroupCapacity
        || definition.spawnStemLength > kSpawnStemCapacity) {
        return false;
    }
    for (std::size_t index = definition.spawnStemLength; index < kSpawnStemCapacity; ++index) {
        if (definition.spawnStem[index] != '\0') {
            return false;
        }
    }
    for (std::size_t group = 0; group < kDestinationGroupCapacity; ++group) {
        const bool declared = group < definition.rosterGroupCount;
        if (declared && definition.rosterGroups[group] >= groupCount) {
            return false;
        }
        if (!declared && definition.rosterGroups[group] != 0) {
            return false;
        }
    }
    // Storage past the declared name and bubble count must stay zero, or two caches of the same
    // packages could differ byte for byte while meaning the same thing.
    for (std::size_t index = definition.nameLength; index < kNameCapacity; ++index) {
        if (definition.name[index] != '\0') {
            return false;
        }
    }
    for (std::size_t index = 0; index < kBubbleCapacity; ++index) {
        const bool declared = index < definition.bubbleCount;
        if (declared && definition.bubbleStates[index] != kBubbleEnabledByte
            && definition.bubbleStates[index] != kBubbleDisabledByte) {
            return false;
        }
        if (!declared
            && (definition.bubbleStates[index] != 0 || definition.bubbleHashes[index] != 0
                || definition.bubbleStateCounts[index] != 0
                || definition.bubbleMapIndices[index] != 0)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Clears every extracted destination layout and roster group under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
    g_groups.clear();
}

/** Checks that the rows are canonical and uniquely named. */
bool valid(std::span<const Definition> definitions, std::span<const RosterGroup> groups) noexcept {
    if (definitions.size() > kDefinitionCapacity || groups.size() > kRosterGroupCapacity) {
        return false;
    }
    for (std::size_t row = 0; row < groups.size(); ++row) {
        if (!canonical(groups[row])) {
            return false;
        }
    }
    for (std::size_t row = 0; row < definitions.size(); ++row) {
        if (!canonical(definitions[row], groups.size())) {
            return false;
        }
        // A duplicate name would make the destination lookup depend on row order.
        for (std::size_t earlier = 0; earlier < row; ++earlier) {
            if (name_of(definitions[earlier]) == name_of(definitions[row])) {
                return false;
            }
        }
    }
    return true;
}

/** Replaces the extracted destination layouts and their roster groups in one step. */
bool replace(std::span<const Definition> definitions,
             std::span<const RosterGroup> groups) noexcept {
    if (!valid(definitions, groups)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    // Both run, with no short-circuit, so the pair cannot be left half replaced. valid() already
    // checked each against its size, which is the only reason either can refuse.
    const bool storedDefinitions = g_definitions.replace(definitions);
    const bool storedGroups = g_groups.replace(groups);
    return storedDefinitions && storedGroups;
}

/** Copies one roster group by table index. */
bool group(std::size_t index, RosterGroup& group) noexcept {
    group = {};
    const Lock::Shared guard(g_lock);
    const std::span<const RosterGroup> rows = g_groups.rows();
    const bool present = index < rows.size();
    if (present) {
        group = rows[index];
    }
    return present;
}

/** @return Published roster group count. */
std::size_t group_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_groups.count();
}

/** Copies every roster group in extraction order. */
bool snapshot_groups(std::span<RosterGroup> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_groups.snapshot(output, count);
}

/** Finds one destination layout by package name. */
bool find(std::string_view name, Definition& definition) noexcept {
    definition = {};
    if (name.empty() || name.size() > kNameCapacity) {
        return false;
    }
    const Lock::Shared guard(g_lock);
    for (const Definition& row : g_definitions.rows()) {
        if (name_of(row) == name) {
            definition = row;
            return true;
        }
    }
    return false;
}

/** Copies every row in extraction order. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return The number of extracted destination layouts, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::scenarios
