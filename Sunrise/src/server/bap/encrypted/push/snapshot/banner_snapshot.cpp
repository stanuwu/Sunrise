#include <algorithm>
#include <span>

#include "../../../../../middleware/datagen/character_record/character_record_encoder.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace character_record = middleware::datagen::character_record;

} // namespace

/** Builds the family-zero banner anchor and the record for the character it names. */
bool prepare_banner(Scratch& scratch,
                    std::uint64_t familyRootSoid,
                    std::int32_t version,
                    std::uint64_t previousCharacter,
                    Prepared& prepared) noexcept {
    const state::ScopedAccount accountScope(state::account_for_public_root(familyRootSoid));
    const Reservation reservation = reserve_prior(scratch, prepared);
    const state::AccountState account = state::account_snapshot();
    if (reservation.rawWriteOffset > scratch.plaintext.size()) {
        return false;
    }
    // The selected character, or the first one before any pick. The record accepts a snapshot only
    // during the boot burst, so a refusal here costs the whole family for the run.
    const std::uint64_t named = state::account::banner_character_soid(account);
    std::size_t selectedIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == named) {
            selectedIndex = index;
            break;
        }
    }
    if (named == 0 || selectedIndex == account.characterCount) {
        return false;
    }

    middleware::datagen::family4::loadout::ResolvedInstances instances{};
    std::int32_t light = 0;
    if (!middleware::datagen::family4::loadout::resolve_instances(account, selectedIndex, instances)
        || !state::equipment::light::resolution::character_light(account, selectedIndex, light)) {
        return false;
    }

    /** Both family-zero objects are staged together, so raw storage must hold the pair. */
    constexpr std::size_t kTotalSize =
        character_record::kFamily0AnchorSize + character_record::kFamily0RecordSize;
    if (scratch.plaintext.size() - reservation.rawWriteOffset < kTotalSize) {
        return false;
    }
    const auto anchor =
        std::span(scratch.plaintext)
            .subspan(reservation.rawWriteOffset, character_record::kFamily0AnchorSize);
    const auto record =
        std::span(scratch.plaintext)
            .subspan(reservation.rawWriteOffset + character_record::kFamily0AnchorSize,
                     character_record::kFamily0RecordSize);
    const state::CharacterState& character = account.characters[selectedIndex];
    if (!character_record::encode_family0_anchor(account.primarySoid, character.soid, anchor)
        || !character_record::encode_family0(character, instances, light, record)) {
        return false;
    }
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + kTotalSize);
    std::size_t objectCount = 0;
    if (previousCharacter != 0) {
        // The Client holds an objIdx-1 buffer for one character at a time, allocated from the
        // character the anchor names. The record it already holds is released first, or the
        // replacement finds no buffer and the Client tears the whole family down.
        staged.objects[objectCount] = middleware::queuez::Object{
            middleware::datagen::kBannerCharacterObjectId,
            previousCharacter,
            middleware::queuez::Encoding::none,
            {},
        };
        ++objectCount;
    }
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t compressedSize = 0;
    if (!compress_object(scratch,
                         anchor,
                         middleware::datagen::kBannerAnchorObjectId,
                         account.primarySoid,
                         compressedExtent,
                         staged.objects[objectCount],
                         compressedSize)) {
        return false;
    }
    ++objectCount;
    compressedExtent += compressedSize;
    if (!compress_object(scratch,
                         record,
                         middleware::datagen::kBannerCharacterObjectId,
                         character.soid,
                         compressedExtent,
                         staged.objects[objectCount],
                         compressedSize)) {
        return false;
    }
    ++objectCount;
    compressedExtent += compressedSize;
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    // Always a full snapshot, even when a release rides with it. The accept gate disarms the
    // record's expiry timer before it tests this bit, so a body without it leaves a subscribed
    // record stuck until it times out. The prune keeps this message's own headers, release too.
    staged.family = middleware::queuez::Family{
        middleware::datagen::kBannerFamily,
        familyRootSoid,
        version,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, prepared);
}

/** Builds one in-place Family-0 record upsert from an uncommitted equipment after-image. */
bool prepare_character_appearance_refresh(Scratch& scratch,
                                          const queuez::CharacterAppearanceRefresh& refresh,
                                          const state::CharacterState& afterCharacter,
                                          std::size_t characterIndex,
                                          std::uint8_t nativeEquipmentSlot,
                                          bool replaceCharacterRecord,
                                          Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("equip_appearance_reservation");
    }

    state::AccountState account = state::account_snapshot();
    if (refresh.characterSoid == 0 || afterCharacter.soid != refresh.characterSoid
        || characterIndex >= account.characterCount
        || account.characters[characterIndex].soid != refresh.characterSoid
        || !refresh.after.family0Active || refresh.after.family0Character != refresh.characterSoid
        || refresh.after.family4RootSoid == 0
        || account.primarySoid != refresh.after.family4RootSoid) {
        return report_failure("equip_appearance_mutation");
    }
    account.characters[characterIndex] = afterCharacter;
    if (!state::account::valid(account)
        || state::account::selected_character_soid(account) != refresh.characterSoid) {
        return report_failure("equip_appearance_selection");
    }

    middleware::datagen::family4::loadout::ResolvedInstances instances{};
    std::int32_t light = 0;
    if (!middleware::datagen::family4::loadout::resolve_instances(
            account, characterIndex, instances)
        || !state::equipment::light::resolution::character_light(account, characterIndex, light)) {
        return report_failure("equip_appearance_resolve");
    }

    // The banner-facing emblem consumers bind through the Family-0 anchor, not the character
    // record, so an emblem move must touch the unchanged anchor as well. That dirties those
    // consumers without releasing either resident key.
    constexpr std::uint8_t kEmblemEquipmentSlot = 13;
    const bool refreshAnchor = nativeEquipmentSlot == kEmblemEquipmentSlot;
    const std::size_t anchorSize = refreshAnchor ? character_record::kFamily0AnchorSize : 0U;
    const std::size_t rawSize = anchorSize + character_record::kFamily0RecordSize;
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (rawSize > rawStorage.size()) {
        return report_failure("equip_appearance_storage");
    }
    const auto anchor = rawStorage.first(anchorSize);
    const auto record = rawStorage.subspan(anchorSize, character_record::kFamily0RecordSize);
    if ((refreshAnchor
         && !character_record::encode_family0_anchor(
             account.primarySoid, refresh.characterSoid, anchor))
        || !character_record::encode_family0(afterCharacter, instances, light, record)) {
        return report_failure("equip_appearance_encode");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + rawSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t objectCount = 0;
    if (replaceCharacterRecord) {
        staged.objects[objectCount++] = middleware::queuez::Object{
            middleware::datagen::kBannerCharacterObjectId,
            refresh.characterSoid,
            middleware::queuez::Encoding::none,
            {},
        };
    }
    if (!append_object(scratch,
                       record,
                       middleware::datagen::kBannerCharacterObjectId,
                       refresh.characterSoid,
                       staged.objects[objectCount++],
                       compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("equip_appearance_object");
    }
    // On an incremental refresh the record is already resident. Publish its new body first and
    // touch the anchor second, so an anchor-driven banner observer resolves the new emblem rather
    // than the prior record during the same family update.
    if (refreshAnchor
        && !append_object(scratch,
                          anchor,
                          middleware::datagen::kBannerAnchorObjectId,
                          account.primarySoid,
                          staged.objects[objectCount++],
                          compressedExtent)) {
        clear_after(scratch, reservation);
        return report_failure("equip_appearance_anchor");
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        middleware::datagen::kBannerFamily,
        refresh.after.family4RootSoid,
        refresh.after.family0Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return report_failure("equip_appearance_commit");
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
