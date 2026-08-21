#include "queuez_outcome_staging.h"

#include <limits>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {
namespace {

/**
 * Says whether equipping into one slot changes what the family-two member record publishes.
 *
 * That record carries two fields fed by different slots. The emblem comes from the emblem slot,
 * and the light is the mean of the eight gear slots -- the three weapons and the five armour
 * pieces -- so an armour swap moves the record just as surely as an emblem swap does. Gating on
 * the emblem alone would leave the roster row holding a stale light until the next subscribe.
 *
 * Everything else is excluded because it moves neither field: a ghost, sparrow, ship, subclass,
 * clan banner, emote or finisher carries no Power in this season and is not the emblem, so a
 * swap there would spend a re-push republishing an unchanged object.
 *
 * @param equipmentSlotIndex Authored semantic slot the equip targeted.
 * @return True when the slot feeds the emblem or the light the member record carries.
 */
[[nodiscard]] constexpr bool moves_social_roster(std::size_t equipmentSlotIndex) noexcept {
    namespace inventory = state::account::inventory;
    return equipmentSlotIndex <= static_cast<std::size_t>(inventory::EquipmentSlot::classItem)
           || equipmentSlotIndex == static_cast<std::size_t>(inventory::EquipmentSlot::emblem);
}

} // namespace

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
    std::uint64_t socialRosterRoot = 0;
    bool armsAbilityRefresh = false;
    const auto* equipment = transaction_if<EquipmentSwapTransaction>(outcome);
    const auto* subclassSelection = transaction_if<SubclassSelectionTransaction>(outcome);
    const auto* itemState = transaction_if<ItemStateTransaction>(outcome);
    const auto* socket = transaction_if<SocketPlugTransaction>(outcome);
    const auto* itemAcquisition = transaction_if<ItemAcquisitionTransaction>(outcome);
    const auto* profileAcquisition = transaction_if<ProfileItemAcquisitionTransaction>(outcome);
    const auto* itemDismantle = transaction_if<ItemDismantleTransaction>(outcome);
    // Set before the branch chain rather than inside the equipment arm. That arm returns early
    // when the staged after-image fails validation, and the equip has already moved State by
    // then -- so the published record is stale on exactly the path the arm never finishes.
    publication.rearmsSocialRosterRepush =
        equipment != nullptr && moves_social_roster(equipment->pending.equipmentSlotIndex);
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
        // The subscribe is the only moment a family-two root arrives. Recorded rather than acted
        // on: the inline answer to this subscribe lands, so nothing is owed until an equip makes
        // what it published stale.
        if (outcome.subscription.familyType == kSocialRosterFamilyType) {
            socialRosterRoot = outcome.subscription.familyRootSoid;
        }
    } else if (outcome.hasUnsubscription) {
        stage_unsubscription(before, outcome.unsubscription.familyRootSoid, after);
    } else if (outcome.hasChangeCharacter) {
        // The reply already carries the version this patch promises. A patch that cannot be built
        // leaves the ladder where it is, instead of holding back that reply.
        if (!push::append_change_character_notification(
                scratch, outcome.changeCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=change result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.changeCharacter.after;
    } else if (equipment != nullptr) {
        // Body processing already staged this exact after-image so the correlated opcode-403
        // response could promise its version. Reuse it here; staging a second revision would make
        // the response and pushed Family-4 ladder disagree.
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
            // The State transaction must not commit when the Client cannot receive its after-image.
            return false;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = swap.after;
        // Swapping the subclass slot invalidates the published ability buckets the same way an
        // opcode-801 pick does; the rebuild is likewise asynchronous, so this owes the same
        // delayed re-derivation rather than risking a race with whatever refresh runs below.
        if (equipment->pending.equipmentSlotIndex
            == static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass)) {
            armsAbilityRefresh = true;
        }
        // Family four drives inventory placement, while Family zero owns the rendered appearance
        // the cosmetic panels and world player consume. Its resident character record is updated
        // in place: releasing and re-adding the same key tears down the ship/banner binding.
        if (after.family0Active) {
            CharacterAppearanceRefresh refresh{};
            if (!stage_character_appearance_refresh(
                    after, equipment->pending.characterSoid, refresh)
                || !push::append_equipment_appearance_refresh_notification(
                    scratch, refresh, equipment->pending, key, nonce, response, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=equip_appearance result=fail");
                // The State transaction and both peer ladders remain unpublished when either
                // member of the paired Family-4/Family-0 delivery cannot fit.
                return false;
            }
            after = refresh.after;
        }
        // Family three owns the orbit roster and a separate copy of the same appearance record.
        // Equipment movement changes both bodies, so publish character first and roster second at
        // one exact +1 Family-3 revision.  Failure keeps all three peer ladders and State private.
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
        // Item-state bits live in the selected-character inventory row. Publish only that
        // resident character body; item-instance, appearance, roster and manifest are unchanged.
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
        // Body processing already staged the exact +1 revision opcode 801 promised. The instance
        // upsert goes first, then the appearance and roster refreshes, so gameplay reads the new
        // selection now rather than on the next unrelated poll.
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
        // The ability-bucket rebuild runs off the Client content-extraction pump, so the two
        // refreshes below can race it and carry empty buckets. The delayed re-derivation is owed
        // either way.
        armsAbilityRefresh = true;
        // A subclass is always equipped, so both the appearance and roster ability reads are
        // always owed a refresh once one is active.
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
        // Body processing staged this exact +1 revision before encoding opcode 903's status pair.
        // A socket selection changes only one already-resident item-instance body.
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
        // Equipped plugs feed Family zero's material, overflow-hash and sandbox-perk banks. An
        // inventory-only socket change has no rendered character record to refresh.
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
        // A socket change can alter the rendered shader/perk banks in Family three, but it does not
        // change the roster's base-definition references.  Only the character record is owed.
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
        // Body processing staged this exact manifest append before encoding the response version.
        // The character and new item objects must both fit or the State insertion is not committed.
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
        // A source-backed profile append creates one dependency before the account starts naming
        // it. Existing stacks and non-actionable currency rows preserve the complete manifest.
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
        // A dismantle removes exactly one resident while preserving the relative order of every
        // survivor. The character after-image and empty release descriptor must fit together or
        // the State removal is not committed.
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
        // The reply is the Client's task completion and the move is a separate frame. A move that
        // cannot be built leaves the selection where it is, instead of holding back that reply.
        if (!push::append_select_character_notification(
                scratch, outcome.selectCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=select result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.selectCharacter.after;
        // The banner pair follows the family-four move, so it is sent against the state that move
        // made. A pair that cannot be built leaves the emblem where it is.
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
        // A family-zero subscribe that arrived before this pick was held, not answered. The pick
        // is the first moment the pair can be built, and the peer will not ask again.
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
    // The frame is already written by here. A mirror that fails validation is logged and dropped,
    // never turned into a refusal to send what the Client waits for.
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
    publication.socialRosterRepushRoot = socialRosterRoot;
    publication.armsAbilityRefresh = armsAbilityRefresh;
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
