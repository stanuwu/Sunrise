#include <algorithm>
#include <span>

#include "../../../../../middleware/datagen/character_record/character_record_encoder.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace character_record = middleware::datagen::character_record;

/** The anchor and the record it names are the two objects every family-zero frame upserts. */
constexpr std::size_t kBannerUpsertCount = 2;

} // namespace

/** Builds the family-zero banner anchor and the record for the character it names. */
bool prepare_banner(Scratch& scratch,
                    std::uint64_t familyRootSoid,
                    std::int32_t version,
                    std::uint64_t previousCharacter,
                    Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    const state::AccountState account = state::account_snapshot();
    if (reservation.rawWriteOffset > scratch.plaintext.size()) {
        return false;
    }
    std::size_t selectedIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            selectedIndex = index;
            break;
        }
    }
    if (selectedIndex == account.characterCount) {
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
            middleware::queuez::Encoding::raw,
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
    staged.family = middleware::queuez::Family{
        middleware::datagen::kBannerFamily,
        familyRootSoid,
        version,
        previousCharacter != 0 ? std::uint8_t{0} : middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    static_assert(kBannerUpsertCount == 2);
    return commit(staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
