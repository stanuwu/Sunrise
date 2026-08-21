#include "queuez_outcome_staging.h"

#include <array>
#include <cstdio>
#include <limits>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../push/queuez/queuez_update_frame.h"
#include "../push/snapshot/snapshot.h"
#include "character_creation_publication.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {

/** Stages queuez subscription, unsubscription, or character-move output for one peer. */
bool stage_service_outcome(Scratch& scratch,
                           const SessionState& before,
                           const ServiceOutcome& outcome,
                           std::span<const std::byte, state::kAesKeySize> key,
                           std::array<std::byte, state::kBapNonceSize>& nonce,
                           std::span<std::byte> response,
                           std::size_t& written,
                           StagedPublication& publication) noexcept {
    publication = {};
    SessionState after{};
    bool armsRepush = false;
    bool armsBannerRepush = false;
    std::uint64_t bannerRoot = 0;
    bool armsAbilityRefresh = false;
    const auto* characterCreation = transaction_if<CharacterCreationTransaction>(outcome);
    const auto* characterDeletion = transaction_if<CharacterDeletionTransaction>(outcome);
    const auto* equipment = transaction_if<EquipmentSwapTransaction>(outcome);
    const auto* subclassSelection = transaction_if<SubclassSelectionTransaction>(outcome);
    const auto* itemState = transaction_if<ItemStateTransaction>(outcome);
    const auto* socket = transaction_if<SocketPlugTransaction>(outcome);
    const auto* itemAcquisition = transaction_if<ItemAcquisitionTransaction>(outcome);
    const auto* profileAcquisition = transaction_if<ProfileItemAcquisitionTransaction>(outcome);
    const auto* itemDismantle = transaction_if<ItemDismantleTransaction>(outcome);
    if (outcome.hasSubscription) {
        push::append_queuez_notification(scratch,
                                         before,
                                         outcome.subscription,
                                         key,
                                         nonce,
                                         response,
                                         written,
                                         after,
                                         armsRepush,
                                         armsBannerRepush);
        bannerRoot = outcome.subscription.familyRootSoid;
    } else if (outcome.hasUnsubscription) {
        stage_unsubscription(before, outcome.unsubscription.familyRootSoid, after);
    } else if (outcome.hasChangeCharacter) {
        if (!push::append_change_character_notification(
                scratch, outcome.changeCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=change result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.changeCharacter.after;
    } else if (characterCreation != nullptr) {
        state::AccountState accountAfter{};
        if (!state::preview_character_creation(characterCreation->pending, accountAfter)
            || !character_creation::append(scratch,
                                           before,
                                           characterCreation->pending,
                                           accountAfter,
                                           key,
                                           nonce,
                                           response,
                                           written,
                                           after)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_create result=fail");
            return false;
        }
    } else if (characterDeletion != nullptr) {
        state::AccountState accountAfter{};
        if (!state::preview_character_deletion(characterDeletion->pending, accountAfter)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete result=fail reason=preview");
            return false;
        }
        // Deletion can be accepted before Family 4 is subscribed. In the normal character-select
        // path it is already active, and a next-version full refresh immediately releases the
        // deleted character's item residents and republishes the account's new character count.
        if (!before.family4Active) {
            return true;
        }
        if (before.family4RootSoid != characterDeletion->pending.accountSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete result=fail reason=family4_state");
            return false;
        }

        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_family4_refresh_from_account(
                scratch,
                before.family4RootSoid,
                before.family4Version + 1,
                accountAfter,
                prepared)
            || !stage_family4_refresh(before, prepared.family, after)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete result=fail reason=prepare");
            return false;
        }

        const std::size_t objectCount = prepared.family.objects.size();
        const std::size_t beforeBytes = written;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response,
                                        written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete result=fail reason=frame");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);

        std::array<char, core::log::kLineCapacity> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=queuez stage=character_delete result=ok family=4 objects=%zu bytes=%zu "
            "version=%d residents=%u",
            objectCount,
            written - beforeBytes,
            after.family4Version,
            static_cast<unsigned>(after.family4ResidentCount));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(count)});
        }
    } else if (equipment != nullptr) {
        const EquipmentSwap& swap = equipment->update;
        if (!valid(swap.after) || swap.characterSoid != equipment->pending.characterSoid
            || swap.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || swap.after.family4Version != before.family4Version + 1
            || !push::append_equipment_swap_notification(
                scratch, swap, equipment->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=equip result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = swap.after;
        if (equipment->pending.equipmentSlotIndex
            == static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass)) {
            armsAbilityRefresh = true;
        }
        if (after.family0Active) {
            CharacterAppearanceRefresh refresh{};
            if (!stage_character_appearance_refresh(
                    after, equipment->pending.characterSoid, refresh)
                || !push::append_equipment_appearance_refresh_notification(
                    scratch, refresh, equipment->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=equip_appearance result=fail");
                return false;
            }
            after = refresh.after;
        }
        if (after.family3Active) {
            RosterAppearanceRefresh refresh{};
            if (!stage_roster_appearance_refresh(
                    after, equipment->pending.characterSoid, true, refresh)
                || !push::append_equipment_roster_refresh_notification(
                    scratch, refresh, equipment->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=equip_roster result=fail");
                return false;
            }
            after = refresh.after;
        }
    } else if (itemState != nullptr) {
        const EquipmentSwap& update = itemState->update;
        bool preservedManifest = update.after.family4ResidentCount == before.family4ResidentCount;
        for (std::size_t index = 0; preservedManifest && index < before.family4ResidentCount;
             ++index) {
            preservedManifest = update.after.family4Residents[index].objectSoid
                                    == before.family4Residents[index].objectSoid
                                && update.after.family4Residents[index].definitionId
                                       == before.family4Residents[index].definitionId;
        }
        if (!valid(update.after) || !preservedManifest
            || update.characterSoid != itemState->pending.characterSoid
            || update.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || update.after.family4Version != before.family4Version + 1
            || !push::append_item_state_notification(
                scratch, update, itemState->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=item_state result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = update.after;
    } else if (subclassSelection != nullptr) {
        const SubclassSelection& selection = subclassSelection->update;
        bool preservedManifest =
            selection.after.family4ResidentCount == before.family4ResidentCount;
        std::size_t targetMatches = 0;
        for (std::size_t index = 0; preservedManifest && index < before.family4ResidentCount;
             ++index) {
            const ResidentObject& resident = before.family4Residents[index];
            const ResidentObject& staged = selection.after.family4Residents[index];
            preservedManifest = staged.objectSoid == resident.objectSoid
                                && staged.definitionId == resident.definitionId;
            targetMatches += static_cast<std::size_t>(
                resident.objectSoid == selection.subclassInstanceSoid
                && resident.definitionId == selection.itemInstanceDefinitionId);
        }
        if (!valid(selection.after) || !preservedManifest || targetMatches != 1
            || selection.accountSoid != subclassSelection->pending.accountSoid
            || selection.characterSoid != subclassSelection->pending.characterSoid
            || selection.subclassInstanceSoid != subclassSelection->pending.subclassInstanceSoid
            || selection.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || selection.after.family4Version != before.family4Version + 1
            || !push::append_subclass_selection_notification(
                scratch, selection, subclassSelection->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=subclass_select result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = selection.after;
        armsAbilityRefresh = true;
        if (after.family0Active) {
            CharacterAppearanceRefresh refresh{};
            if (!stage_character_appearance_refresh(
                    after, subclassSelection->pending.characterSoid, refresh)
                || !push::append_subclass_appearance_refresh_notification(
                    scratch, refresh, subclassSelection->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=subclass_appearance result=fail");
                return false;
            }
            after = refresh.after;
        }
        if (after.family3Active) {
            RosterAppearanceRefresh refresh{};
            if (!stage_roster_appearance_refresh(
                    after, subclassSelection->pending.characterSoid, false, refresh)
                || !push::append_subclass_roster_refresh_notification(
                    scratch, refresh, subclassSelection->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=subclass_roster result=fail");
                return false;
            }
            after = refresh.after;
        }
    } else if (socket != nullptr) {
        const SocketPlug& socketPlug = socket->update;
        bool preservedManifest =
            socketPlug.after.family4ResidentCount == before.family4ResidentCount;
        std::size_t accountMatches = 0;
        std::size_t targetMatches = 0;
        for (std::size_t index = 0; preservedManifest && index < before.family4ResidentCount;
             ++index) {
            const ResidentObject& resident = before.family4Residents[index];
            const ResidentObject& staged = socketPlug.after.family4Residents[index];
            preservedManifest = staged.objectSoid == resident.objectSoid
                                && staged.definitionId == resident.definitionId;
            targetMatches += static_cast<std::size_t>(
                resident.objectSoid == socketPlug.targetInstanceSoid
                && resident.definitionId == socketPlug.itemInstanceDefinitionId);
            accountMatches += static_cast<std::size_t>(resident.objectSoid == socketPlug.accountSoid
                                                       && resident.definitionId
                                                              == socketPlug.accountDefinitionId);
        }
        if (!valid(socketPlug.after) || !preservedManifest || accountMatches != 1
            || targetMatches != 1 || socketPlug.accountSoid != socket->pending.accountSoid
            || socketPlug.characterSoid != socket->pending.characterSoid
            || socketPlug.targetInstanceSoid != socket->pending.targetInstanceSoid
            || socketPlug.updatesAccount != socket->pending.profileChanged
            || socketPlug.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || socketPlug.after.family4Version != before.family4Version + 1
            || !push::append_socket_plug_notification(
                scratch, socketPlug, socket->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=socket_plug result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = socketPlug.after;
        if (socket->pending.targetEquipped && after.family0Active) {
            CharacterAppearanceRefresh refresh{};
            if (!stage_character_appearance_refresh(after, socket->pending.characterSoid, refresh)
                || !push::append_socket_appearance_refresh_notification(
                    scratch, refresh, socket->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=socket_appearance result=fail");
                return false;
            }
            after = refresh.after;
        }
        if (socket->pending.targetEquipped && after.family3Active) {
            RosterAppearanceRefresh refresh{};
            if (!stage_roster_appearance_refresh(
                    after, socket->pending.characterSoid, false, refresh)
                || !push::append_socket_roster_refresh_notification(
                    scratch, refresh, socket->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=socket_roster result=fail");
                return false;
            }
            after = refresh.after;
        }
    } else if (itemAcquisition != nullptr) {
        const ItemAcquisition& acquisition = itemAcquisition->update;
        const std::size_t appendedIndex = before.family4ResidentCount;
        bool preservedManifest = acquisition.after.family4ResidentCount == appendedIndex + 1U;
        for (std::size_t index = 0; preservedManifest && index < appendedIndex; ++index) {
            preservedManifest = acquisition.after.family4Residents[index].objectSoid
                                    == before.family4Residents[index].objectSoid
                                && acquisition.after.family4Residents[index].definitionId
                                       == before.family4Residents[index].definitionId;
        }
        if (!valid(acquisition.after) || !preservedManifest
            || acquisition.accountSoid != itemAcquisition->pending.accountSoid
            || acquisition.characterSoid != itemAcquisition->pending.characterSoid
            || acquisition.acquiredInstanceSoid != itemAcquisition->pending.acquiredInstanceSoid
            || acquisition.updatesAccount != itemAcquisition->pending.profileChanged
            || acquisition.accountSoid != before.family4RootSoid
            || acquisition.after.family4RootSoid != before.family4RootSoid
            || before.family4ResidentCount >= before.family4Residents.size()
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || acquisition.after.family4Version != before.family4Version + 1
            || acquisition.after.family4Residents[appendedIndex].objectSoid
                   != acquisition.acquiredInstanceSoid
            || acquisition.after.family4Residents[appendedIndex].definitionId
                   != acquisition.itemInstanceDefinitionId
            || !push::append_item_acquisition_notification(
                scratch, acquisition, itemAcquisition->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=acquire result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = acquisition.after;
    } else if (profileAcquisition != nullptr) {
        const ProfileItemAcquisition& acquisition = profileAcquisition->update;
        const std::size_t priorResidentCount = before.family4ResidentCount;
        const std::size_t expectedResidentCount =
            priorResidentCount + static_cast<std::size_t>(acquisition.appendedResident);
        bool validManifest = expectedResidentCount <= acquisition.after.family4Residents.size()
                             && acquisition.after.family4ResidentCount == expectedResidentCount;
        for (std::size_t index = 0; validManifest && index < priorResidentCount; ++index) {
            validManifest = acquisition.after.family4Residents[index].objectSoid
                                == before.family4Residents[index].objectSoid
                            && acquisition.after.family4Residents[index].definitionId
                                   == before.family4Residents[index].definitionId;
        }
        std::size_t priorProfileResidentMatches = 0;
        for (std::size_t index = 0; index < priorResidentCount; ++index) {
            const ResidentObject& resident = before.family4Residents[index];
            priorProfileResidentMatches += static_cast<std::size_t>(
                acquisition.acquiredInstanceSoid != 0
                && resident.objectSoid == acquisition.acquiredInstanceSoid
                && resident.definitionId == acquisition.itemInstanceDefinitionId);
        }
        const bool appendedResidentValid =
            !acquisition.appendedResident
            || (priorResidentCount < acquisition.after.family4Residents.size()
                && acquisition.after.family4Residents[priorResidentCount].objectSoid
                       == acquisition.acquiredInstanceSoid
                && acquisition.after.family4Residents[priorResidentCount].definitionId
                       == acquisition.itemInstanceDefinitionId
                && priorProfileResidentMatches == 0);
        const bool sourceIdentityValid =
            acquisition.actionSource == (acquisition.acquiredInstanceSoid != 0)
            && (acquisition.actionSource
                    ? acquisition.itemInstanceDefinitionId != 0 && appendedResidentValid
                          && (acquisition.appendedResident || priorProfileResidentMatches == 1)
                    : acquisition.itemInstanceDefinitionId == 0 && !acquisition.appendedResident
                          && priorProfileResidentMatches == 0);
        if (!valid(acquisition.after) || !validManifest || !sourceIdentityValid
            || acquisition.accountSoid != profileAcquisition->pending.accountSoid
            || acquisition.acquiredInstanceSoid != profileAcquisition->pending.acquiredInstanceSoid
            || acquisition.actionSource != profileAcquisition->pending.actionSource
            || acquisition.appendedResident
                   != (profileAcquisition->pending.appended
                       && profileAcquisition->pending.actionSource)
            || acquisition.accountSoid != before.family4RootSoid || before.family4ResidentCount == 0
            || acquisition.accountDefinitionId != before.family4Residents.front().definitionId
            || acquisition.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || acquisition.after.family4Version != before.family4Version + 1
            || !push::append_profile_item_acquisition_notification(
                scratch, acquisition, profileAcquisition->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=profile_acquire result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = acquisition.after;
    } else if (itemDismantle != nullptr) {
        const ItemDismantle& dismantle = itemDismantle->update;
        bool compactedManifest =
            before.family4ResidentCount != 0
            && dismantle.after.family4ResidentCount + 1U == before.family4ResidentCount;
        std::size_t afterIndex = 0;
        std::size_t removedCount = 0;
        for (std::size_t beforeIndex = 0;
             compactedManifest && beforeIndex < before.family4ResidentCount;
             ++beforeIndex) {
            const ResidentObject& resident = before.family4Residents[beforeIndex];
            if (resident.objectSoid == dismantle.dismantledInstanceSoid) {
                compactedManifest = resident.definitionId == dismantle.itemInstanceDefinitionId;
                ++removedCount;
                continue;
            }
            if (afterIndex >= dismantle.after.family4ResidentCount) {
                compactedManifest = false;
                break;
            }
            const ResidentObject& survivor = dismantle.after.family4Residents[afterIndex++];
            compactedManifest = survivor.objectSoid == resident.objectSoid
                                && survivor.definitionId == resident.definitionId;
        }
        compactedManifest = compactedManifest && removedCount == 1U
                            && afterIndex == dismantle.after.family4ResidentCount;

        if (!valid(dismantle.after) || !compactedManifest
            || dismantle.accountSoid != itemDismantle->pending.accountSoid
            || dismantle.characterSoid != itemDismantle->pending.characterSoid
            || dismantle.dismantledInstanceSoid != itemDismantle->pending.dismantledInstanceSoid
            || dismantle.updatesAccount != itemDismantle->pending.profileChanged
            || dismantle.accountSoid != before.family4RootSoid || before.family4ResidentCount == 0
            || dismantle.accountDefinitionId != before.family4Residents.front().definitionId
            || dismantle.after.family4RootSoid != before.family4RootSoid
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || dismantle.after.family4Version != before.family4Version + 1
            || !push::append_item_dismantle_notification(
                scratch, dismantle, itemDismantle->pending, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=dismantle result=fail");
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = dismantle.after;
    } else if (outcome.hasSelectCharacter) {
        if (!push::append_select_character_notification(
                scratch, outcome.selectCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=select result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.selectCharacter.after;
        const SessionState& bannerBefore = after;
        SessionState bannerAfter{};
        if (push::append_banner_move_notification(scratch,
                                                  bannerBefore,
                                                  outcome.selectCharacter.selectedCharacterSoid,
                                                  key,
                                                  nonce,
                                                  response,
                                                  written,
                                                  bannerAfter)) {
            after = bannerAfter;
        }
        if (after.pendingBannerRoot != 0) {
            middleware::queuez::Subscription held{};
            held.familyType = kBannerFamilyType;
            held.familyRootSoid = after.pendingBannerRoot;
            SessionState heldAfter{};
            bool heldRepush = false;
            bool heldBannerRepush = false;
            const SessionState heldBefore = after;
            push::append_queuez_notification(scratch,
                                             heldBefore,
                                             held,
                                             key,
                                             nonce,
                                             response,
                                             written,
                                             heldAfter,
                                             heldRepush,
                                             heldBannerRepush);
            if (valid(heldAfter)) {
                after = heldAfter;
            }
            if (heldBannerRepush) {
                armsBannerRepush = true;
                bannerRoot = held.familyRootSoid;
            }
        }
    } else {
        return true;
    }
    publication.hasState = valid(after);
    if (publication.hasState) {
        publication.after = after;
    } else {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=publish result=unrecorded");
    }
    publication.armsFamily4Repush = armsRepush;
    publication.family4RepushRoot = armsRepush ? outcome.subscription.familyRootSoid : 0;
    publication.armsBannerRepush = armsBannerRepush && bannerRoot != 0;
    publication.bannerRepushRoot = publication.armsBannerRepush ? bannerRoot : 0;
    publication.armsAbilityRefresh = armsAbilityRefresh;
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
