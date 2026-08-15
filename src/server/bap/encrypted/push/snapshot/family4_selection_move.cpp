#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/account/selection_patch/\
account_selection_patch_encoder.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace family4_datagen = middleware::datagen::family4;
namespace selection_patch = middleware::datagen::family4::account::selection_patch;

} // namespace

/** Builds the Family-4 increment that moves the character object to the picked character. */
bool prepare_selection_move(Scratch& scratch,
                            const queuez::SelectCharacter& select,
                            Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("move_reservation");
    }
    const state::AccountState account = state::account_snapshot();
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    if (!state::account::valid(account) || !selectedIndex.has_value()
        || !resolve(account, *selectedIndex, selected)
        || account.characters[selected.characterIndex].soid != select.selectedCharacterSoid) {
        return report_failure("move_selection");
    }

    Prepared staged{};
    staged.rawClearSize = reservation.rawClearSize;
    staged.compressedClearSize = reservation.compressedClearSize;
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t objectCount = 0;
    // The first select resends the character object even when the pick names the current one. With
    // no pending change all three operations go out. Only a pending change may skip it, and only
    // when the character did not move.
    if (!select.patchAccount || select.previousCharacterSoid != select.selectedCharacterSoid) {
        // A zero previous key means the snapshot published no character object. There is no slot
        // to release, so the pick adds one. Releasing a key the Client never held raises queuez
        // error 4.
        if (select.previousCharacterSoid != 0) {
            // The release carries no payload. The encoding field is never read for an empty
            // object, so it just matches its neighbours.
            staged.objects[objectCount++] = middleware::queuez::Object{
                select.characterDefinitionId,
                select.previousCharacterSoid,
                middleware::queuez::Encoding::oodle,
                {},
            };
        }
        if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
            return report_failure("move_character_storage");
        }
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        if (!family4_datagen::character::encode(account.characters[selected.characterIndex],
                                                selected.loadout,
                                                selected.lightEvaluation,
                                                characterBytes)) {
            return report_failure("move_character_object");
        }
        if (!append_object(scratch,
                           characterBytes,
                           select.characterDefinitionId,
                           select.selectedCharacterSoid,
                           staged.objects[objectCount++],
                           compressedExtent)) {
            return report_failure("move_character_object");
        }
        staged.rawClearSize = (std::max)(staged.rawClearSize,
                                         reservation.rawWriteOffset
                                             + family4_datagen::character::layout::kObjectSize);
    }

    // The character object is already compressed into sealed storage, so the raw span is free
    // for whichever account body this move carries.
    if (select.patchAccount) {
        std::size_t patchSize = 0;
        if (!selection_patch::encode(select.selectedCharacterSoid, rawStorage, patchSize)
            || patchSize != selection_patch::kPayloadSize) {
            return report_failure("move_account_patch");
        }
        staged.objects[objectCount++] = middleware::queuez::Object{
            select.accountDefinitionId,
            select.after.family4RootSoid,
            middleware::queuez::Encoding::tagReflection,
            rawStorage.first(patchSize),
        };
        staged.rawClearSize =
            (std::max)(staged.rawClearSize, reservation.rawWriteOffset + patchSize);
    } else {
        if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
            return report_failure("move_account_storage");
        }
        const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
        if (!family4_datagen::account::encode(account, accountBytes)) {
            return report_failure("move_account_object");
        }
        if (!append_object(scratch,
                           accountBytes,
                           select.accountDefinitionId,
                           select.after.family4RootSoid,
                           staged.objects[objectCount++],
                           compressedExtent)) {
            return report_failure("move_account_object");
        }
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        select.after.family4RootSoid,
        select.after.family4Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("move_commit");
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
