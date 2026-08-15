#include "item_detail_catalog.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <span>

#include "../../table.h"
#include "../item_catalog.h"

namespace sunrise::state::build_data::items::details {
namespace {

/** A 16-bit native definition index can address 65,536 rows. */
constexpr std::size_t kNativeDefinitionIndexCapacity =
    static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()) + 1U;
/** An all-one row marks a native index with no published configured detail. */
constexpr std::uint16_t kEmptyLookupRow = (std::numeric_limits<std::uint16_t>::max)();

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;
// Native definition index to detail row, rebuilt with the table under the same exclusive hold.
std::array<std::uint16_t, kNativeDefinitionIndexCapacity> g_lookup{};

static_assert(kDefinitionCapacity < kEmptyLookupRow);

/** @return True when the record's equipment slot is missing or valid. */
[[nodiscard]] bool equipment_slot_valid(const Definition& definition) noexcept {
    if (!definition.equipmentSlot.has_value()) {
        return true;
    }
    const std::int8_t slot = *definition.equipmentSlot;
    return slot >= 0 && static_cast<std::size_t>(slot) < kEquipmentSlotCount;
}

/** @return True for either of the two known quantity states. */
[[nodiscard]] bool instance_state_valid(InstancedDefinitionState state) noexcept {
    return state == InstancedDefinitionState::stackable
           || state == InstancedDefinitionState::instanced;
}

/**
 * Checks the socket state, the count, and the unused tail.
 * @param definition Candidate configured item detail.
 * @return True when the fixed socket array has one of the supported shapes.
 */
[[nodiscard]] bool ordinary_sockets_valid(const Definition& definition) noexcept {
    if (definition.ordinarySocketCount > kInitialPlugCapacity) {
        return false;
    }

    std::size_t tailStart = 0;
    switch (definition.ordinarySocketState) {
    case OrdinarySocketState::absent:
        if (definition.ordinarySocketCount != 0) {
            return false;
        }
        break;
    case OrdinarySocketState::present:
        tailStart = definition.ordinarySocketCount;
        break;
    default:
        return false;
    }

    const auto unusedTail = std::span(definition.initialPlugIndices).subspan(tailStart);
    return std::all_of(unusedTail.begin(), unusedTail.end(), [](std::uint16_t index) {
        return index == kUnavailableItemIndex;
    });
}

/** @return True when every structure field of the record is valid. */
[[nodiscard]] bool definition_valid(const Definition& definition) noexcept {
    return definition.bucketId != items::kUnresolvedBucketId && definition.maxStackSize > 0
           && instance_state_valid(definition.instancedDefinitionState)
           && equipment_slot_valid(definition) && ordinary_sockets_valid(definition);
}

} // namespace

/** Clears every generated configured item detail under the catalog lock. */
void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
    std::fill(g_lookup.begin(), g_lookup.end(), kEmptyLookupRow);
}

/** Checks the detail fields and that every native index is unique. */
bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }

    std::bitset<kNativeDefinitionIndexCapacity> occupied{};
    for (const Definition& definition : definitions) {
        if (occupied.test(definition.definitionIndex) || !definition_valid(definition)) {
            return false;
        }
        occupied.set(definition.definitionIndex);
    }
    return true;
}

/** Rebuilds the detail table and its native-index lookup after the checks pass. */
bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }

    const Lock::Exclusive guard(g_lock);
    std::fill(g_lookup.begin(), g_lookup.end(), kEmptyLookupRow);
    if (!g_definitions.replace(definitions)) {
        return false;
    }
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        g_lookup[definitions[index].definitionIndex] = static_cast<std::uint16_t>(index);
    }
    return true;
}

/** Finds one configured item detail by native definition index. */
bool find(std::uint16_t definitionIndex, Definition& definition) noexcept {
    definition = {};
    const Lock::Shared guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    const std::uint16_t row = g_lookup[definitionIndex];
    const bool found = row != kEmptyLookupRow && row < rows.size();
    if (found) {
        definition = rows[row];
    }
    return found;
}

/** Copies details in publication order, without exposing the catalog storage. */
bool snapshot(std::span<Definition> output, std::size_t& count) noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.snapshot(output, count);
}

/** @return Number of configured item details, read under the lock. */
std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::items::details
