#include "account_state.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sunrise::state::account {

/**
 * Checks bounded ids and whole settings for every nonzero account.
 * @param state Account State to check.
 * @return True for empty State, or a whole account with unique ids.
 */
bool valid(const AccountState& state) noexcept {
    if (state.primarySoid == 0) {
        // Null accounts cannot own characters or a detached settings object.
        return state.characterCount == 0 && !state.settings.configured
               && !state.settings.keyBindings.configured;
    }
    if (state.characterCount > state.characters.size() || !settings::valid(state.settings)) {
        return false;
    }
    bool selected = false;
    std::array<std::uint64_t,
               kCharacterCapacity
                   * (inventory::kEquipmentSlotCount + inventory::kCharacterInventoryCapacity)>
        itemSoids{};
    std::size_t itemSoidCount = 0;
    for (std::size_t index = 0; index < state.characterCount; ++index) {
        const CharacterState& character = state.characters[index];
        if (character.soid == 0 || character.soid == state.primarySoid
            || (character.selected && selected) || character.race > CharacterRace::exo
            || character.gender > CharacterGender::female
            || character.characterClass > CharacterClass::warlock
            || !std::isfinite(character.appearanceValue)
            || !inventory::valid(character.equipment) || !inventory::valid(character.inventory)) {
            return false;
        }
        selected = selected || character.selected;
        // The fixed 3-slot size keeps this compare against earlier entries bounded and
        // allocation-free.
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.characters[prior].soid == character.soid) {
                return false;
            }
        }
        for (const std::optional<inventory::Item>& item : character.equipment.slots) {
            if (!item.has_value()) {
                continue;
            }
            const auto end = itemSoids.cbegin() + static_cast<std::ptrdiff_t>(itemSoidCount);
            if (std::find(itemSoids.cbegin(), end, item->instanceSoid) != end) {
                return false;
            }
            itemSoids[itemSoidCount++] = item->instanceSoid;
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.itemCount; ++itemIndex) {
            const inventory::Item& item = character.inventory.items[itemIndex];
            const auto end = itemSoids.cbegin() + static_cast<std::ptrdiff_t>(itemSoidCount);
            if (std::find(itemSoids.cbegin(), end, item.instanceSoid) != end) return false;
            itemSoids[itemSoidCount++] = item.instanceSoid;
        }
    }
    return true;
}

/**
 * Finds the selected character without storing a second account-level key.
 * @param state Account snapshot read under the lock.
 * @return The selected character's nonzero SOID, or zero when none is selected.
 */
std::uint64_t selected_character_soid(const AccountState& state) noexcept {
    const std::size_t count = (std::min)(state.characterCount, state.characters.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (state.characters[index].selected) {
            return state.characters[index].soid;
        }
    }
    return 0;
}

} // namespace sunrise::state::account
