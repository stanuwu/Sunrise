#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace family4_datagen = middleware::datagen::family4;

/**
 * Publishes the staged family metadata once every needed object is done.
 * @param subscription Family id the Client picked.
 * @param objectCount Descriptors staged for this family.
 * @param compressedExtent Size of the used prefix of the sealed buffer.
 * @param reservation Cleanup extent carried over from any prior live snapshot.
 * @param staged Descriptors and clear extents built by the caller.
 * @param output Gets the family snapshot only on success.
 * @return True when the staged descriptors pass the ownership check.
 */
[[nodiscard]] bool publish(const middleware::queuez::Subscription& subscription,
                           std::size_t objectCount,
                           std::size_t compressedExtent,
                           const Reservation& reservation,
                           Prepared& staged,
                           Prepared& output) noexcept {
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        subscription.familyRootSoid,
        kInitialFamilyVersion,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, output);
}

} // namespace

/** Builds the Family-4 account, selected-character, and item-instance snapshot. */
bool prepare(Scratch& scratch,
             const middleware::queuez::Subscription& subscription,
             std::uint32_t accountObjectId,
             const Reservation& reservation,
             Prepared& prepared) noexcept {
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("reservation");
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
        return report_failure("account_storage");
    }
    // Package extraction may have completed after State initialization on a first cache build.
    // Canonicalize profile action-source identities immediately before the first Family-4 image.
    if (!state::ensure_profile_item_identities()) {
        return report_failure("profile_identities");
    }
    // The emote-collection canonicalization deliberately does not live here. Family zero and
    // family three build their own images from the same account and neither passes through this
    // function, so it runs in the shared preflight ahead of the whole dispatch instead
    // (push::ensure_account_canonical).
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account)) {
        return report_failure("account_state");
    }

    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    if (selectedIndex.has_value() && !resolve(account, *selectedIndex, selected)) {
        return report_failure("selection");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)) {
        return report_failure("account_object");
    }
    if (!append_object(scratch,
                       accountBytes,
                       accountObjectId,
                       account.primarySoid,
                       staged.objects[kAccountObjectIndex],
                       compressedExtent)) {
        return report_failure("account_object");
    }
    // The character object is the only descriptor a selection owns. With no selection it is absent
    // and the items move up behind the account object, keeping the published prefix contiguous.
    const std::size_t itemBaseIndex =
        selectedIndex.has_value() ? kFirstItemObjectIndex : kFirstItemObjectIndexUnselected;
    if (selectedIndex.has_value()) {
        staged.rawClearSize = (std::max)(staged.rawClearSize,
                                         reservation.rawWriteOffset
                                             + family4_datagen::character::layout::kObjectSize);
        if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
            return report_failure("character_storage");
        }
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        const state::CharacterState& selectedCharacter =
            account.characters[selected.characterIndex];
        if (!family4_datagen::character::encode(
                selectedCharacter, selected.loadout, selected.lightEvaluation, characterBytes)) {
            return report_failure("character_encode");
        }
        if (!append_object(scratch,
                           characterBytes,
                           selected.characterObjectId,
                           selectedCharacter.soid,
                           staged.objects[kCharacterObjectIndex],
                           compressedExtent)) {
            return report_failure("character_object");
        }
    }
    // Every character in the roster needs its item records. The equip-summary reader looks up an
    // instance with no null check, so a missing record is a null read.
    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("item_object_id");
    }
    std::size_t itemCursor = 0;
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        family4_datagen::loadout::ResolvedInstances instances{};
        if (!family4_datagen::loadout::resolve_owned_instances(
                account, characterIndex, instances)) {
            return report_failure("loadout");
        }
        if (instances.itemCount != 0
            && !append_items(scratch,
                             rawStorage,
                             itemInstanceObjectId,
                             instances,
                             itemBaseIndex,
                             staged,
                             itemCursor,
                             compressedExtent)) {
            return report_failure("items");
        }
    }
    // Profile rows with nonzero SOIDs are native action sources.  Their item residents follow all
    // character-owned instances so the full-snapshot manifest remains deterministic.
    if (!append_profile_items(scratch,
                              rawStorage,
                              itemInstanceObjectId,
                              account,
                              itemBaseIndex,
                              staged,
                              itemCursor,
                              compressedExtent)) {
        return report_failure("profile_items");
    }

    const std::size_t objectCount = itemBaseIndex + itemCursor;
    if (itemCursor != 0) {
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    }
    return publish(subscription, objectCount, compressedExtent, reservation, staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
