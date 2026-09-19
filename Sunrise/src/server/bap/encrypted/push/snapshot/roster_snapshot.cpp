#include <algorithm>
#include <span>

#include "../../../../../middleware/datagen/character_record/character_record_encoder.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family3/family3_roster.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace character_record = middleware::datagen::character_record;

/**
 * Appends one family-three character record per character in use.
 * The roster names every character, and the select screen reads a record for each one.
 * @param scratch Raw object storage owned by the lock.
 * @param account Account State read under the lock.
 * @param rawExtent First unused raw byte, advanced for every record.
 * @param compressedExtent First unused sealed byte, advanced for every record.
 * @param staged Snapshot that takes one descriptor per character.
 * @param objectCount Descriptors already staged, advanced for every record.
 * @return True when every character is found and its record fits raw storage.
 */
[[nodiscard]] bool append_character_records(Scratch& scratch,
                                            const state::AccountState& account,
                                            std::size_t& rawExtent,
                                            std::size_t& compressedExtent,
                                            Prepared& staged,
                                            std::size_t& objectCount) noexcept {
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        middleware::datagen::family4::loadout::ResolvedInstances instances{};
        std::int32_t light = 0;
        if (!middleware::datagen::family4::loadout::resolve_instances(account, index, instances)
            || !state::equipment::light::resolution::character_light(account, index, light)) {
            return false;
        }
        if (rawExtent > scratch.plaintext.size()
            || scratch.plaintext.size() - rawExtent < character_record::kFamily3RecordSize
            || objectCount >= staged.objects.size()) {
            return false;
        }
        const auto record =
            std::span(scratch.plaintext).subspan(rawExtent, character_record::kFamily3RecordSize);
        if (!character_record::encode_family3(
                account.characters[index], instances, light, record)) {
            return false;
        }
        std::size_t compressedSize = 0;
        if (!compress_object(scratch,
                             record,
                             middleware::datagen::kRosterCharacterObjectId,
                             account.characters[index].soid,
                             compressedExtent,
                             staged.objects[objectCount],
                             compressedSize)) {
            return false;
        }
        compressedExtent += compressedSize;
        ++objectCount;
        rawExtent += character_record::kFamily3RecordSize;
    }
    return true;
}

} // namespace

/** Builds the family-three roster snapshot and one record per character. */
bool prepare_roster(Scratch& scratch,
                    const middleware::queuez::Subscription& subscription,
                    std::uint32_t objectId,
                    const Reservation& reservation,
                    Prepared& prepared) noexcept {
    const state::ScopedAccountView accountScope(
        state::account_for_subscription_root(subscription.familyRootSoid));
    const state::AccountState account = state::bound_account_snapshot();
    if (reservation.rawWriteOffset > scratch.plaintext.size()) {
        return false;
    }
    const auto destination = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    std::size_t rawSize = 0;
    if (!middleware::datagen::family3::encode_roster(account, destination, rawSize)) {
        return false;
    }
    Prepared staged{};
    std::size_t objectCount = 0;
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    // Every queuez object goes out Oodle-encoded. The Client has no raw-object path here.
    std::size_t listCompressed = 0;
    if (!compress_object(scratch,
                         destination.first(rawSize),
                         objectId,
                         account.primarySoid,
                         compressedExtent,
                         staged.objects[objectCount],
                         listCompressed)) {
        return false;
    }
    compressedExtent += listCompressed;
    ++objectCount;
    std::size_t rawExtent = reservation.rawWriteOffset + rawSize;
    if (!append_character_records(
            scratch, account, rawExtent, compressedExtent, staged, objectCount)) {
        return false;
    }
    staged.rawClearSize = (std::max)(reservation.rawClearSize, rawExtent);
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        subscription.familyType,
        subscription.familyRootSoid,
        kInitialFamilyVersion,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, prepared);
}

/** Builds one incremental Family-3 character record and optional changed account roster. */
bool prepare_roster_appearance_refresh(Scratch& scratch,
                                       const queuez::RosterAppearanceRefresh& refresh,
                                       const state::CharacterState& afterCharacter,
                                       std::size_t characterIndex,
                                       Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return false;
    }

    state::AccountState account = state::bound_account_snapshot();
    if (!refresh.after.family3Active || refresh.after.family3RootSoid == 0
        || refresh.after.family3Version <= kInitialFamilyVersion || refresh.characterSoid == 0
        || afterCharacter.soid != refresh.characterSoid || characterIndex >= account.characterCount
        || account.characters[characterIndex].soid != refresh.characterSoid
        || account.primarySoid != refresh.after.family3RootSoid) {
        return false;
    }
    account.characters[characterIndex] = afterCharacter;
    if (!state::account::valid(account)
        || state::account::selected_character_soid(account) != refresh.characterSoid) {
        return false;
    }

    middleware::datagen::family4::loadout::ResolvedInstances instances{};
    std::int32_t light = 0;
    if (!middleware::datagen::family4::loadout::resolve_instances(
            account, characterIndex, instances)
        || !state::equipment::light::resolution::character_light(account, characterIndex, light)) {
        return false;
    }

    const std::size_t rosterSize =
        refresh.includeRoster ? middleware::datagen::family3::kRosterSize : 0U;
    const std::size_t rawSize = character_record::kFamily3RecordSize + rosterSize;
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (rawSize > rawStorage.size()) {
        return false;
    }
    const auto characterBytes = rawStorage.first(character_record::kFamily3RecordSize);
    if (!character_record::encode_family3(afterCharacter, instances, light, characterBytes)) {
        return false;
    }
    std::span<std::byte> rosterBytes{};
    if (refresh.includeRoster) {
        rosterBytes = rawStorage.subspan(character_record::kFamily3RecordSize, rosterSize);
        std::size_t encodedRosterSize = 0;
        if (!middleware::datagen::family3::encode_roster(account, rosterBytes, encodedRosterSize)
            || encodedRosterSize != rosterSize) {
            return false;
        }
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + rawSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t objectCount = 0;
    if (!append_object(scratch,
                       characterBytes,
                       middleware::datagen::kRosterCharacterObjectId,
                       refresh.characterSoid,
                       staged.objects[objectCount++],
                       compressedExtent)) {
        clear_after(scratch, reservation);
        return false;
    }
    if (refresh.includeRoster
        && !append_object(scratch,
                          rosterBytes,
                          middleware::datagen::kRosterObjectId,
                          account.primarySoid,
                          staged.objects[objectCount++],
                          compressedExtent)) {
        clear_after(scratch, reservation);
        return false;
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kRosterFamilyType,
        refresh.after.family3RootSoid,
        refresh.after.family3Version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    if (!commit(staged, prepared)) {
        clear_after(scratch, reservation);
        return false;
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
