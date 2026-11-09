#include <algorithm>
#include <array>
#include <cstring>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/presence/presence_record_encoder.h"
#include "../../../../../state/account/public_profiles.h"
#include "../../../../../state/activity/fireteam.h"
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
/** One native roster record. The client fills it from its fireteam group session 0. */
struct RosterRecord final {
    std::uint64_t soid;
    /** Emblem of the member's selected character, or the absent definition. */
    std::uint16_t emblem;
    /** Always the absent definition; the member record serves it that way. */
    std::uint16_t absent;
    std::array<char16_t, 64> name;
    std::uint8_t flags;
    std::array<std::byte, 3> padding;
};
#pragma pack(pop)

/** Records in the block; its tail past them is the publishing client's own live state. */
constexpr std::size_t kRosterRecords = 20;
static_assert(sizeof(RosterRecord) == 144);
static_assert(kRosterRecords * sizeof(RosterRecord) < state::social::kNativeFireteamSize);
/** Record flags: a fireteam member, resolved to an account, and the block's own publisher. */
constexpr std::uint8_t kFireteamMember = 0x01;
constexpr std::uint8_t kTranslated = 0x02;
constexpr std::uint8_t kPublisher = 0x08;

/** Builds one member's record from its public account. @return False when it is unpublished. */
[[nodiscard]] bool roster_record(Scratch& scratch,
                                 state::AccountHandle handle,
                                 bool publisher,
                                 RosterRecord& output) noexcept {
    const state::ScopedAccountView bind(handle);
    auto& member = scratch.accountDecode;
    if (!state::bound_account_snapshot(member) || member.primarySoid == 0) {
        return false;
    }
    output = {};
    output.soid = member.primarySoid;
    output.emblem = middleware::datagen::presence::kAbsentDefinition;
    output.absent = middleware::datagen::presence::kAbsentDefinition;
    std::uint32_t emblemHash = 0;
    read_selected_emblem(member, output.emblem, emblemHash);
    const auto& persona = member.presence.personaName[0] != '\0' ? member.presence.personaName
                                                                 : member.presence.displayName;
    for (std::size_t i = 0; i + 1 < output.name.size() && i < persona.size() && persona[i]; ++i) {
        output.name[i] = static_cast<char16_t>(static_cast<unsigned char>(persona[i]));
    }
    output.flags =
        static_cast<std::uint8_t>(kFireteamMember | kTranslated | (publisher ? kPublisher : 0U));
    return true;
}

/**
 * Overwrites the block's records with the publisher's fireteam as the server holds it: the
 * publisher first, then every account connected to it. The tail stays the client's.
 */
void write_roster(Scratch& scratch,
                  std::uint64_t publisherSoid,
                  state::AccountHandle publisher,
                  std::array<std::byte, state::social::kNativeFireteamSize>& block) noexcept {
    std::array<RosterRecord, kRosterRecords> rows{};
    std::size_t count = roster_record(scratch, publisher, true, rows[0]) ? 1 : 0;
    for (state::AccountHandle handle = 0; handle < state::account_count() && count < rows.size();
         ++handle) {
        const std::uint64_t soid = state::account_primary_soid(handle);
        if (handle != publisher && soid != 0
            && state::activity::fireteam::connected(publisherSoid, soid)
            && roster_record(scratch, handle, false, rows[count])) {
            ++count;
        }
    }
    std::memcpy(block.data(), rows.data(), sizeof rows);
}
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
    if (published) {
        write_roster(scratch, account.primarySoid, handle, payload);
    }
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
