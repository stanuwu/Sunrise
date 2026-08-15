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
    const state::AccountState account = state::account_snapshot();
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

} // namespace sunrise::server::bap::encrypted::push::snapshot
