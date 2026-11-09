#include <algorithm>
#include <array>
#include <cstring>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/gameplay/descriptor/net_addr.h"
#include "../../../../../state/build_data/cache/internal.h"
#include "../../../../../state/build_data/platform_metadata.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../../activity_transport_publication.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {
namespace descriptor = middleware::gameplay::descriptor;

bool join_silo(std::uint64_t& silo) noexcept {
    state::build_data::BuildIdentity build{};
    // This service runs inside the game process, whose own image owns the native join silo.
    // The silo keys on the image fields alone, so neither hash is needed.
    return state::build_data::cache::current_build_identity(0, 0, build)
           && state::build_data::static_join_silo(build, silo);
}

bool native_descriptor(const state::AccountState& account,
                       std::uint64_t& silo,
                       std::array<std::byte, descriptor::kDescriptorSize>& body) noexcept {
    const auto& native = account.presence.native;
    const auto character = state::account::selected_character_soid(account);
    if (!native.published || character == 0 || native.characterSoid != character
        || native.descriptorSize != descriptor::kDescriptorSize) {
        return false;
    }
    std::uint64_t fireteamHash{}, session{};
    std::memcpy(&fireteamHash, native.descriptor.data(), sizeof fireteamHash);
    std::memcpy(&session,
                native.descriptor.data() + state::social::kNativeJoinSessionOffset,
                sizeof session);
    if (fireteamHash == 0 || session == 0) {
        return false;
    }
    std::array<std::byte, descriptor::kNetAddrSize> own{}, chosen{};
    std::copy_n(native.descriptor.begin() + state::social::kNativeJoinAddressOffset,
                own.size(),
                own.begin());
    body = native.descriptor;
    // The two blobs the same client published about itself, and the descriptor's own NetAddr.
    // A published carrier wins over the descriptor's own bytes: the type-12 row and the group
    // snapshot both name this peer by that carrier, and naming it by different bytes in each
    // registers a second security context that clears the channel's security flag if it never
    // becomes ready.
    middleware::bap::activity_message::TransportReport transport{};
    const bool hasPublishedTransport =
        published_character_transport_locked(account.primarySoid, character, transport);
    auto rule = descriptor::NetAddrNormalisation::unavailable;
    bool unified = false;
    if (hasPublishedTransport) {
        rule = descriptor::normalize_net_addr_ipv4(transport.address, transport.alternate, chosen);
        unified = rule != descriptor::NetAddrNormalisation::unavailable;
    }
    if (!unified) {
        rule = descriptor::normalize_net_addr_ipv4(own, transport.address, chosen);
        if (rule == descriptor::NetAddrNormalisation::unavailable) {
            rule = descriptor::normalize_net_addr_ipv4(own, transport.alternate, chosen);
        }
        // Without an independent published carrier, accept only the descriptor's own direct
        // address verbatim. A Steam or text address still needs a real IPv4 carrier.
        if (!hasPublishedTransport && rule != descriptor::NetAddrNormalisation::kept) {
            rule = descriptor::NetAddrNormalisation::unavailable;
        }
    }
    if (rule == descriptor::NetAddrNormalisation::unavailable) {
        return false;
    }
    // Only the address changes. The client still owns fireteam/session IDs and opaque join keys.
    std::copy(chosen.begin(), chosen.end(), body.begin() + state::social::kNativeJoinAddressOffset);
    return join_silo(silo);
}

#pragma pack(push, 1)
/** The family-seven directory: this account's key and the link naming its descriptor. */
struct JoinDirectory final {
    std::uint64_t key;
    std::uint64_t descriptorLink;
};
/** The descriptor object; the client reads the static join silo last. */
struct JoinDescriptorRecord final {
    std::uint64_t key;
    /** Declared byte length of `body`. */
    std::uint32_t length;
    std::array<std::byte, descriptor::kDescriptorSize> body;
    std::array<std::byte, 4> alignment;
    std::uint64_t silo;
};
#pragma pack(pop)
} // namespace

bool prepare_join_descriptor(Scratch& scratch,
                             const middleware::queuez::Subscription& subscription,
                             const Reservation& reservation,
                             Prepared& prepared) noexcept {
    namespace datagen = middleware::datagen;
    constexpr std::size_t directorySize = sizeof(JoinDirectory);
    constexpr std::size_t descriptorSize = sizeof(JoinDescriptorRecord);
    auto& account = scratch.accountImage;
    const auto handle = state::account_for_subscription_root(subscription.familyRootSoid);
    const state::ScopedAccountView bind(handle);
    if (subscription.familyType != datagen::kJoinFamily || !state::bound_account_snapshot(account)
        || account.primarySoid != subscription.familyRootSoid
        || reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return false;
    }
    std::uint64_t silo{};
    std::array<std::byte, descriptor::kDescriptorSize> body{};
    const bool published = native_descriptor(account, silo, body);
    const auto rawSize = directorySize + (published ? descriptorSize : 0);
    if (scratch.plaintext.size() - reservation.rawWriteOffset < rawSize) {
        return false;
    }
    auto raw = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset, rawSize);
    std::fill(raw.begin(), raw.end(), std::byte{});
    const JoinDirectory directory{account.primarySoid, account.primarySoid};
    std::memcpy(raw.data(), &directory, sizeof directory);
    Prepared staged{};
    std::size_t compressed = reservation.compressedWriteOffset, size{};
    if (!compress_object(scratch,
                         raw.first(directorySize),
                         datagen::kJoinDirectoryObjectId,
                         account.primarySoid,
                         compressed,
                         staged.objects[0],
                         size)) {
        return false;
    }
    compressed += size;
    std::size_t count = 1;
    if (published) {
        const auto record = raw.subspan(directorySize, descriptorSize);
        JoinDescriptorRecord wire{};
        wire.key = account.primarySoid;
        wire.length = static_cast<std::uint32_t>(descriptor::kDescriptorSize);
        wire.body = body;
        wire.silo = silo;
        std::memcpy(record.data(), &wire, sizeof wire);
        if (!compress_object(scratch,
                             record,
                             datagen::kJoinDescriptorObjectId,
                             account.primarySoid,
                             compressed,
                             staged.objects[1],
                             size)) {
            return false;
        }
        compressed += size;
        ++count;
    }
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + rawSize);
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressed);
    staged.family = {datagen::kJoinFamily,
                     account.primarySoid,
                     kInitialFamilyVersion,
                     middleware::queuez::kFullSnapshotFlag,
                     std::span(staged.objects).first(count)};
    return commit(staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
