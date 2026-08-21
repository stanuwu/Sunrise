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

/** Publishes staged family metadata once every needed object is done. */
[[nodiscard]] bool publish(const middleware::queuez::Subscription& subscription,
                           std::int32_t version,
                           std::size_t objectCount,
                           std::size_t compressedExtent,
                           const Reservation& reservation,
                           Prepared& staged,
                           Prepared& output) noexcept {
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        subscription.familyRootSoid,
        version,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, output);
}

/** Builds one complete Family-4 image from the supplied canonical account. */
[[nodiscard]] bool prepare_account(Scratch& scratch,
                                   const middleware::queuez::Subscription& subscription,
                                   std::uint32_t accountObjectId,
                                   std::int32_t version,
                                   const state::AccountState& account,
                                   const Reservation& reservation,
                                   Prepared& prepared) noexcept {
    if (subscription.familyType != kAccountFamilyType || subscription.familyRootSoid == 0
        || account.primarySoid != subscription.familyRootSoid || !state::account::valid(account)
        || reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("account_input");
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
        return report_failure("account_storage");
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

    // The selected-character object is optional; all characters' item residents are not.
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
    return publish(subscription,
                   version,
                   objectCount,
                   compressedExtent,
                   reservation,
                   staged,
                   prepared);
}

} // namespace

/** Builds the Family-4 account, selected-character, and item-instance snapshot from live State. */
bool prepare(Scratch& scratch,
             const middleware::queuez::Subscription& subscription,
             std::uint32_t accountObjectId,
             const Reservation& reservation,
             Prepared& prepared) noexcept {
    // Package extraction may have completed after State initialization on a first cache build.
    // Canonicalize profile action-source identities immediately before the first Family-4 image.
    if (!state::ensure_profile_item_identities()) {
        return report_failure("profile_identities");
    }
    const state::AccountState account = state::account_snapshot();
    return prepare_account(scratch,
                           subscription,
                           accountObjectId,
                           kInitialFamilyVersion,
                           account,
                           reservation,
                           prepared);
}

/** Builds a complete next-version Family-4 snapshot from an uncommitted account after-image. */
bool prepare_family4_refresh_from_account(Scratch& scratch,
                                          std::uint64_t familyRootSoid,
                                          std::int32_t version,
                                          const state::AccountState& account,
                                          Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion
        || account.primarySoid != familyRootSoid || !state::account::valid(account)) {
        return false;
    }
    std::uint32_t accountObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kAccountDefinitionSlotIndex, accountObjectId)) {
        return false;
    }
    middleware::queuez::Subscription subscription{};
    subscription.familyType = kAccountFamilyType;
    subscription.familyRootSoid = familyRootSoid;
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (!prepare_account(scratch,
                         subscription,
                         accountObjectId,
                         version,
                         account,
                         reservation,
                         prepared)) {
        clear_after(scratch, reservation);
        return false;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
