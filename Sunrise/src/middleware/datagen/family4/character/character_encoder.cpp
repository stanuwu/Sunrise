#include "../../../../state/record_claims/record_claims.h"
#include "../../../../state/build_data/nodes/node_catalog.h"
#include <atomic>

#include "../../../../state/build_data/nodes/node_persistence.h"
#include "character_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

#include "../../../../state/unlocks/unlocks_runtime.h"
#include "../instance/layout.h"
#include "../progression/progression_bank_keys.h"
#include "abi.h"
#include "equipment_summary_builder.h"
#include "layout.h"

namespace sunrise::middleware::datagen::family4::character {
namespace {

/** Every bit set is the native empty biased 16-bit definition index. */
constexpr std::uint16_t kEmptyDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Every set bit marks all default per-character messages as already seen. */
constexpr std::byte kSeenMessageByte{0xFF};
/** Native 1-byte booleans encode true as 1. */
constexpr std::uint8_t kNativeTrue = 1;
/** Native 1-byte booleans encode false as 0. */
constexpr std::uint8_t kNativeFalse = 0;

/**
 * Validates the authored fields consumed by the selected-character encoder.
 * @param state Candidate character identity and policy state.
 * @return True when every encoded scalar fits its stable State domain.
 */
[[nodiscard]] bool valid(const state::CharacterState& state) noexcept {
    return state.soid != 0 && state.race <= state::CharacterRace::exo
           && state.gender <= state::CharacterGender::female
           && state.characterClass <= state::CharacterClass::warlock
           && std::isfinite(state.appearanceValue);
}

/** One new-item flag byte covers 8 consecutive inventory rows. */
constexpr std::size_t kBitsPerFlagByte = 8;
/**
 * The watermark an occupied inventory row carries; an empty row keeps 0.
 * This is the character object's own per-row field, not the item instance's roll progress, so it
 * does not share that constant.
 */
constexpr std::int32_t kOccupiedRowWatermark = 1;

/**
 * Validates every inventory and equipment field consumed by the character object.
 * @param resolvedLoadout Candidate row-sorted installed mapping.
 * @return True when rows, slots, SOIDs, indices, quantities, and serials are canonical.
 */
[[nodiscard]] bool valid(const loadout::ResolvedLoadout& resolvedLoadout) noexcept {
    if (resolvedLoadout.itemCount > resolvedLoadout.items.size()
        || resolvedLoadout.nextInventorySerial < resolvedLoadout.itemCount
        || resolvedLoadout.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }

    std::array<bool, layout::kInventoryCapacity> occupiedRows{};
    std::array<bool, layout::kEquipmentCapacity> occupiedEquipmentSlots{};
    std::array<std::uint64_t, loadout::kItemCapacity> instanceSoids{};
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const loadout::ResolvedItem& item = resolvedLoadout.items[index];
        const instance::ResolvedInstance& itemInstance = item.instance;
        const auto priorSoidsEnd = instanceSoids.cbegin() + static_cast<std::ptrdiff_t>(index);
        if (item.inventoryRow >= occupiedRows.size()
            || item.equipmentSlot >= occupiedEquipmentSlots.size() || item.quantity <= 0
            || itemInstance.instanceSoid == 0 || itemInstance.bounds.itemDefinitionCount == 0
            || itemInstance.bounds.itemDefinitionCount > instance::layout::kDefinitionIndexCapacity
            || itemInstance.baseDefinitionIndex == kEmptyDefinitionIndex
            || itemInstance.baseDefinitionIndex >= itemInstance.bounds.itemDefinitionCount
            || item.mutationSerial < 0
            || static_cast<std::uint32_t>(item.mutationSerial)
                   >= resolvedLoadout.nextInventorySerial
            || occupiedRows[item.inventoryRow]
            || (item.equipped && occupiedEquipmentSlots[item.equipmentSlot])
            || std::find(instanceSoids.cbegin(), priorSoidsEnd, itemInstance.instanceSoid)
                   != priorSoidsEnd
            || (index != 0 && resolvedLoadout.items[index - 1].inventoryRow >= item.inventoryRow)) {
            return false;
        }
        occupiedRows[item.inventoryRow] = true;
        if (item.equipped) {
            occupiedEquipmentSlots[item.equipmentSlot] = true;
        }
        instanceSoids[index] = itemInstance.instanceSoid;
    }
    return true;
}

/**
 * Confirms selected-character summary rows describe exactly the resolved equipped instances.
 * @param resolvedLoadout Canonical row-sorted inventory and equipment mappings.
 * @param evaluation Complete semantic equipment-light evaluation.
 * @return True when every equipped native slot carries the same definition index once.
 */
[[nodiscard]] bool
summary_matches_loadout(const loadout::ResolvedLoadout& resolvedLoadout,
                        const state::equipment::light::Evaluation& evaluation) noexcept {
    std::size_t summaryItemCount = 0;
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        summaryItemCount += static_cast<std::size_t>(resolvedLoadout.items[index].equipped);
    }
    std::size_t scoreCount = 0;
    for (const std::optional<state::equipment::light::ItemScore>& score : evaluation.character) {
        scoreCount += static_cast<std::size_t>(score.has_value());
    }
    if (summaryItemCount != scoreCount) {
        return false;
    }
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const loadout::ResolvedItem& item = resolvedLoadout.items[index];
        if (!item.equipped) {
            continue;
        }
        const auto& score = evaluation.character[item.equipmentSlot];
        if (!score.has_value() || score->definitionIndex != item.instance.baseDefinitionIndex) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Encodes one selected-character object from authored State and resolved installed mappings. */
bool encode(const state::CharacterState& state,
            const loadout::ResolvedLoadout& resolvedLoadout,
            const state::equipment::light::Evaluation& lightEvaluation,
            std::span<std::byte> output) noexcept {
    if (!valid(state) || !valid(resolvedLoadout)
        || !summary_matches_loadout(resolvedLoadout, lightEvaluation)
        || output.size() < layout::kObjectSize) {
        return false;
    }

    layout::Object object{};
    object.characterSoid = state.soid;
    object.identity.race = static_cast<std::int8_t>(state.race);
    object.identity.gender = static_cast<std::int8_t>(state.gender);
    object.identity.characterClass = static_cast<std::int8_t>(state.characterClass);
    object.lastOrbitedDestination = state.lastOrbitedDestination;
    object.previewMirrors.fill(state.previewAvailable ? kNativeTrue : kNativeFalse);
    object.contentBypass = state.contentBypass ? kNativeTrue : kNativeFalse;
    object.seenMessages.fill(kSeenMessageByte);
    for (inventory::layout::Entry& item : object.inventoryItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    // Acquired flags and objective progress are authored policy, published once per process.
    const state::unlocks::Table& unlocks = state::unlocks::get();
    for (std::size_t index = 0; index < object.acquiredFlags.size(); ++index) {
        object.acquiredFlags[index] = static_cast<std::byte>(
            index < unlocks.characterObjectFlags.size() ? unlocks.characterObjectFlags[index]
                                                        : std::uint8_t{});
    }
    // A character image is only built once a character is selected, which is later than any
    // account or content path. The node table is published from here on a warm start, where the
    // package pass is skipped and every earlier attempt is refused. Latches on success.
    {
        static std::atomic<bool> nodesPublished{false};
        if (!nodesPublished.load(std::memory_order_relaxed)
            && state::build_data::nodes::load_and_publish()) {
            nodesPublished.store(true, std::memory_order_relaxed);
        }
    }

    // One lore book counts in the character bank rather than the account one.
    (void)state::record_claims::apply_character_node_progress(object.objectiveValues);

    // One lore book's gate is character scoped rather than account scoped.
    (void)state::build_data::nodes::apply_character_visibility(object.acquiredFlags);

    for (std::size_t index = 0; index < object.objectiveValues.size(); ++index) {
        object.objectiveValues[index] =
            index < unlocks.characterObjectValues.size() ? unlocks.characterObjectValues[index] : 0;
    }
    if (!build_equipment_summary(lightEvaluation, object.equipmentSummary)) {
        return false;
    }
    if (!progression::key_bank(state::build_data::progressions::Scope::character,
                               object.progressions)) {
        return false;
    }
    object.nextInventorySerial = resolvedLoadout.nextInventorySerial;
    for (std::size_t index = 0; index < resolvedLoadout.itemCount; ++index) {
        const loadout::ResolvedItem& item = resolvedLoadout.items[index];
        inventory::layout::Entry& inventoryRow = object.inventoryItems[item.inventoryRow];
        inventoryRow.definitionIndex = item.instance.baseDefinitionIndex;
        inventoryRow.instanceSoid = item.instance.instanceSoid;
        inventoryRow.quantity = item.quantity;
        inventoryRow.mutationSerial = item.mutationSerial;
        inventoryRow.flags = item.flags;
        // Both companion arrays are indexed by inventory row, not by equipment slot, and the
        // client's own producer marks every row it fills.
        object.newItemFlags[item.inventoryRow / kBitsPerFlagByte] |=
            std::byte{1U} << (item.inventoryRow % kBitsPerFlagByte);
        object.instanceProgressWatermarks[item.inventoryRow] = kOccupiedRowWatermark;
        if (item.equipped) {
            object.equippedInstanceSoids[item.equipmentSlot] = item.instance.instanceSoid;
        }
    }

    // Commit only after validation so callers never receive a partially initialized object.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::character
