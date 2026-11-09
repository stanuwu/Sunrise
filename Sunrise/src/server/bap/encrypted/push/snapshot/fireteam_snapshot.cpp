#include <algorithm>
#include <array>
#include <cstring>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../state/account/public_profiles.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../../../state/social/fireteam_projection.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {
#pragma pack(push, 1)
/** The family-six directory: this account's key and the character naming its descriptor. */
struct FireteamDirectory final {
    std::uint64_t key;
    std::uint64_t character;
};
/** The family-six descriptor: the character key, then its native fireteam block. */
struct FireteamDescriptor final {
    std::uint64_t character;
    std::array<std::byte, state::social::kNativeFireteamSize> fireteam;
};
#pragma pack(pop)
} // namespace

bool prepare_fireteam(Scratch& scratch,
                      const middleware::queuez::Subscription& subscription,
                      const Reservation& reservation,
                      Prepared& prepared) noexcept {
    namespace datagen = middleware::datagen;
    namespace social = state::social;
    constexpr std::size_t directorySize = sizeof(FireteamDirectory);
    constexpr std::size_t descriptorSize = sizeof(FireteamDescriptor);
    auto& account = scratch.accountImage;
    const auto handle = state::account_for_subscription_root(subscription.familyRootSoid);
    const state::ScopedAccountView bind(handle);
    if (subscription.familyType != datagen::kFireteamFamily
        || !state::bound_account_snapshot(account)
        || account.primarySoid != subscription.familyRootSoid
        || reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return false;
    }
    const auto character = state::account::selected_character_soid(account);
    // The selected character and its publication come from one account snapshot.
    const std::array publications{
        social::NativePublication{account.primarySoid, account.presence.native}};
    std::array<std::byte, social::kNativeFireteamSize> payload{};
    const bool published = character != 0 && account.presence.native.characterSoid == character
                           && social::project_fireteam(account.primarySoid, publications, payload);
    const auto rawSize = directorySize + (published ? descriptorSize : 0);
    if (scratch.plaintext.size() - reservation.rawWriteOffset < rawSize) {
        return false;
    }
    auto raw = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset, rawSize);
    std::fill(raw.begin(), raw.end(), std::byte{});
    const FireteamDirectory directory{account.primarySoid, character};
    std::memcpy(raw.data(), &directory, sizeof directory);
    Prepared staged{};
    std::size_t compressed = reservation.compressedWriteOffset;
    std::size_t size{};
    if (!compress_object(scratch,
                         raw.first(directorySize),
                         datagen::kFireteamDirectoryObjectId,
                         account.primarySoid,
                         compressed,
                         staged.objects[0],
                         size)) {
        return false;
    }
    compressed += size;
    std::size_t objectCount = 1;
    if (published) {
        const auto descriptor = raw.subspan(directorySize, descriptorSize);
        const FireteamDescriptor wire{character, payload};
        std::memcpy(descriptor.data(), &wire, sizeof wire);
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
                     account.primarySoid,
                     kInitialFamilyVersion,
                     middleware::queuez::kFullSnapshotFlag,
                     std::span(staged.objects).first(objectCount)};
    return commit(staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
