#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../state/account/public_profiles.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../../state/social/fireteam_projection.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

bool prepare_fireteam(Scratch& scratch,
                      const middleware::queuez::Subscription& subscription,
                      const Reservation& reservation,
                      Prepared& prepared) noexcept {
    namespace datagen = middleware::datagen;
    namespace social = state::social;
    constexpr std::size_t directorySize = 16;
    constexpr std::size_t descriptorSize = 8 + social::kNativeFireteamSize;
    static_assert(descriptorSize == 0xB68);
    auto account = std::unique_ptr<state::AccountState>(new (std::nothrow) state::AccountState{});
    const auto handle = state::account_for_subscription_root(subscription.familyRootSoid);
    const state::ScopedAccountView bind(handle);
    if (subscription.familyType != datagen::kFireteamFamily || !account
        || !state::bound_account_snapshot(*account)
        || account->primarySoid != subscription.familyRootSoid
        || reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return false;
    }
    const auto character = state::account::selected_character_soid(*account);
    // The selected character and its publication come from one account snapshot.
    const std::array publications{
        social::NativePublication{account->primarySoid, account->presence.native}};
    std::array<std::byte, social::kNativeFireteamSize> payload{};
    const bool published = character != 0 && account->presence.native.characterSoid == character
                           && social::project_fireteam(account->primarySoid, publications, payload);
    const auto rawSize = directorySize + (published ? descriptorSize : 0);
    if (scratch.plaintext.size() - reservation.rawWriteOffset < rawSize) {
        return false;
    }
    auto raw = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset, rawSize);
    std::fill(raw.begin(), raw.end(), std::byte{});
    std::memcpy(raw.data(), &account->primarySoid, 8);
    std::memcpy(raw.data() + 8, &character, 8);
    Prepared staged{};
    std::size_t compressed = reservation.compressedWriteOffset;
    std::size_t size{};
    if (!compress_object(scratch,
                         raw.first(directorySize),
                         datagen::kFireteamDirectoryObjectId,
                         account->primarySoid,
                         compressed,
                         staged.objects[0],
                         size)) {
        return false;
    }
    compressed += size;
    std::size_t objectCount = 1;
    if (published) {
        const auto descriptor = raw.subspan(directorySize, descriptorSize);
        std::memcpy(descriptor.data(), &character, 8);
        std::copy(payload.begin(), payload.end(), descriptor.begin() + 8);
        if (!compress_object(scratch,
                             descriptor,
                             datagen::kFireteamDescriptorObjectId,
                             character,
                             compressed,
                             staged.objects[1],
                             size)) {
            return false;
        }
        compressed += size;
        ++objectCount;
    }
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + rawSize);
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressed);
    staged.family = {datagen::kFireteamFamily,
                     account->primarySoid,
                     kInitialFamilyVersion,
                     middleware::queuez::kFullSnapshotFlag,
                     std::span(staged.objects).first(objectCount)};
    return commit(staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
