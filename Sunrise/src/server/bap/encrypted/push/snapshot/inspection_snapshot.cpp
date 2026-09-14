#include <algorithm>
#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../middleware/datagen/inspection/inspection_encoder.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

/**
 * Names the predicate that refused one inspection profile.
 * An unserved family falls back to an empty full snapshot with no other explanation, so the
 * refusal is otherwise silent in a boot log.
 * @param reason One word naming the failing step.
 * @param root Account root the subscription named.
 * @return Always false, so callers can return it directly.
 */
[[nodiscard]] bool refuse(const char* reason, std::uint64_t root) noexcept {
    std::array<char, 128> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=queuez stage=inspection result=empty reason=%s family=1 root=0x%016llX",
                      reason,
                      static_cast<unsigned long long>(root));
    if (count > 0 && static_cast<std::size_t>(count) < line.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return false;
}

} // namespace

bool prepare_inspection(Scratch& scratch,
                        const middleware::queuez::Subscription& subscription,
                        const Reservation& reservation,
                        Prepared& prepared) noexcept {
    namespace inspection = middleware::datagen::inspection;
    namespace datagen = middleware::datagen;
    const std::uint64_t root = subscription.familyRootSoid;
    // The connection is the viewer. Every nested account/progression/gear lookup must instead use
    // the root being inspected, just as the peer banner does. Unknown roots stay empty.
    state::AccountHandle handle = state::kInvalidAccount;
    if (!state::account_handle_for_soid(root, handle)) {
        return refuse("account_handle", root);
    }
    const state::ScopedAccount bind{handle};
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid_public(account)) {
        return refuse("account_public", root);
    }
    if (account.primarySoid != root) {
        return refuse("account_root", root);
    }
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return refuse("reservation", root);
    }
    // The roster's character key names the banner character even before an explicit pick.
    const auto characterSoid = state::account::banner_character_soid(account);
    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == characterSoid) {
            characterIndex = index;
            break;
        }
    }
    if (characterSoid == 0 || characterIndex == account.characterCount) {
        return refuse("banner_character", root);
    }
    datagen::family4::loadout::ResolvedLoadout loadout{};
    datagen::family4::loadout::ResolvedInstances equipped{};
    std::int32_t light = 0;
    if (!datagen::family4::loadout::resolve_character(account, characterIndex, loadout)) {
        return refuse("loadout", root);
    }
    if (!inspection::equipped_instances(loadout, equipped)) {
        return refuse("equipped_instances", root);
    }
    if (!state::equipment::light::resolution::character_light(account, characterIndex, light)) {
        return refuse("light", root);
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (rawStorage.size() < inspection::kCharacterSize) {
        return refuse("raw_storage", root);
    }
    Prepared staged{};
    staged.rawClearSize = (std::max)(reservation.rawClearSize,
                                     reservation.rawWriteOffset + inspection::kCharacterSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    const auto rootBytes = rawStorage.first(inspection::kRootSize);
    if (!inspection::encode_root(account.primarySoid, characterSoid, rootBytes)
        || !append_object(scratch,
                          rootBytes,
                          datagen::kInspectionRootObjectId,
                          account.primarySoid,
                          staged.objects[0],
                          compressedExtent)) {
        return refuse("root_object", root);
    }
    const auto characterBytes = rawStorage.first(inspection::kCharacterSize);
    if (!inspection::encode_character(
            account.characters[characterIndex], loadout, equipped, light, characterBytes)
        || !append_object(scratch,
                          characterBytes,
                          datagen::kInspectionCharacterObjectId,
                          characterSoid,
                          staged.objects[1],
                          compressedExtent)) {
        return refuse("character_object", root);
    }
    std::size_t itemCount = 0;
    if (!append_items(scratch,
                      rawStorage,
                      datagen::kItemInstanceObjectId,
                      equipped,
                      2,
                      staged,
                      itemCount,
                      compressedExtent)) {
        return refuse("item_objects", root);
    }
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{datagen::kInspectionFamily,
                                               account.primarySoid,
                                               kInitialFamilyVersion,
                                               middleware::queuez::kFullSnapshotFlag,
                                               std::span(staged.objects).first(2 + itemCount)};
    if (!commit(staged, prepared)) {
        return refuse("commit", root);
    }
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=queuez stage=inspection_profile result=ok family=1 root=0x%llX character=0x%llX "
        "objects=%zu bytes=%zu items=%zu light=%d",
        static_cast<unsigned long long>(account.primarySoid),
        static_cast<unsigned long long>(characterSoid),
        2 + itemCount,
        compressedExtent - reservation.compressedWriteOffset,
        itemCount,
        light);
    if (count > 0 && static_cast<std::size_t>(count) < line.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
