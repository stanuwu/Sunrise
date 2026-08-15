#include <cstddef>

#include "../../../../../middleware/datagen/definitions.h"
#include "../queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {

/** Stages the account-selection patch without changing the resident manifest. */
bool stage_change_character(const SessionState& before, ChangeCharacter& change) noexcept {
    change = {};
    if (!valid(before) || !before.family4Active || before.family4RootSoid == 0
        || before.family4ResidentCount == 0
        || before.family4ResidentCount > before.family4Residents.size()
        || before.family3Phase != Family3Phase::normal) {
        return false;
    }
    const ResidentObject& account = before.family4Residents.front();
    if (account.definitionId == 0 || account.objectSoid != before.family4RootSoid) {
        return false;
    }
    change.after = before;
    change.after.family4Version = before.family4Version + 1;
    change.after.family3Phase = Family3Phase::publishOnce;
    change.accountDefinitionId = account.definitionId;
    return true;
}

/** Stages the Family-4 increment that moves the character object to the picked character. */
bool stage_select_character(const SessionState& before,
                            std::uint64_t selectedCharacterSoid,
                            SelectCharacter& select) noexcept {
    select = {};
    std::uint32_t accountDefinitionId = 0;
    std::uint32_t characterDefinitionId = 0;
    if (!valid(before) || !before.family4Active || selectedCharacterSoid == 0
        || before.family4RootSoid == 0 || selectedCharacterSoid == before.family4RootSoid
        || before.family4ResidentCount == 0
        || !middleware::datagen::object_id(
            kAccountFamilyType, middleware::datagen::kAccountSlot, accountDefinitionId)
        || !middleware::datagen::object_id(
            kAccountFamilyType, middleware::datagen::kCharacterSlot, characterDefinitionId)) {
        return false;
    }
    const ResidentObject& account = before.family4Residents.front();
    if (account.definitionId != accountDefinitionId
        || account.objectSoid != before.family4RootSoid) {
        return false;
    }
    // The character object is the only resident a selection owns. With nothing selected the
    // snapshot leaves it out and the items sit in its place. Its slot is found by definition, not
    // by position, which is what lets the first pick add it.
    std::size_t characterIndex = before.family4Residents.size();
    for (std::size_t index = 0; index < before.family4ResidentCount; ++index) {
        if (before.family4Residents[index].definitionId == characterDefinitionId) {
            characterIndex = index;
            break;
        }
    }
    const bool seated = characterIndex < before.family4Residents.size();
    if (seated && before.family4Residents[characterIndex].objectSoid == 0) {
        return false;
    }
    // A pick that names the resident character resends no object. Sending the move for it would
    // delete and re-add the same key, which cost the ship and the banner.
    const bool changePending = before.family3Phase != Family3Phase::normal;
    const bool samePick =
        seated && before.family4Residents[characterIndex].objectSoid == selectedCharacterSoid;
    if (samePick && !changePending) {
        return false;
    }
    for (std::size_t index = 0; !samePick && index < before.family4ResidentCount; ++index) {
        if (before.family4Residents[index].objectSoid == selectedCharacterSoid) {
            return false;
        }
    }

    select.after = before;
    select.after.family4Version = before.family4Version + 1;
    if (!seated) {
        // An added resident goes after the ones the snapshot published, so no item resident moves
        // and the account keeps index zero.
        if (before.family4ResidentCount >= before.family4Residents.size()) {
            return false;
        }
        characterIndex = before.family4ResidentCount;
        select.after.family4ResidentCount = before.family4ResidentCount + 1;
        select.after.family4Residents[characterIndex].definitionId = characterDefinitionId;
    }
    select.after.family4Residents[characterIndex].objectSoid = selectedCharacterSoid;
    // The pick is what finishes a selection. The roster is held back only until then, and the
    // next opcode 505 needs the phase back at rest.
    select.after.family3Phase = Family3Phase::normal;
    select.accountDefinitionId = accountDefinitionId;
    select.characterDefinitionId = characterDefinitionId;
    // Zero means there was no resident to release, which is what makes the first pick an add.
    select.previousCharacterSoid = seated ? before.family4Residents[characterIndex].objectSoid : 0;
    select.selectedCharacterSoid = selectedCharacterSoid;
    select.patchAccount = changePending;
    return valid(select.after);
}

} // namespace sunrise::server::bap::encrypted::queuez
