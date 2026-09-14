#include "activity_peer_snapshot.h"

#include "../../../../../middleware/gameplay/descriptor/net_addr.h"
#include "../../../../../state/account/public_profiles.h"
#include "../../../activity_transport_publication.h"

namespace sunrise::server::bap::encrypted::push {
void project_activity_peers(
    const state::activity::reservations::Roster& roster,
    middleware::bap::activity_message::replicate_membership::MembershipSnapshot& output) noexcept {
    namespace descriptor = middleware::gameplay::descriptor;
    static_assert(state::activity::reservations::kPeerCapacity
                  == middleware::bap::activity_message::replicate_membership::kPeerMemberCapacity);
    output.peers = {};
    for (std::size_t i = 0; i < roster.peers.size(); ++i) {
        const auto& identity = roster.peers[i];
        if (!identity.memberKey) {
            continue;
        }
        state::AccountHandle owner{};
        std::array<char, state::kDisplayNameCapacity> name{};
        if (!state::account::profiles::find(identity.accountSoid, owner)
            || !state::account::profiles::selected_member_name(owner, identity.opaqueSoid, name)) {
            continue;
        }
        auto& peer = output.peers[i];
        peer.identity = {identity.memberKey,
                         identity.smallOpaque,
                         identity.signedOpaque,
                         identity.joinIdentity,
                         identity.accountSoid,
                         identity.opaqueSoid,
                         identity.secondaryOpaque};
        peer.present = true;
        for (const char c : name) {
            if (!c || peer.nameLength == peer.name.size()) {
                break;
            }
            peer.name[peer.nameLength++] = static_cast<unsigned char>(c);
        }
        peer.hasName = peer.nameLength != 0;
        middleware::bap::activity_message::TransportReport transport{};
        if (!published_activity_transport_locked(
                identity.accountSoid, identity.opaqueSoid, identity.memberKey, transport)) {
            continue;
        }
        peer.transport.flags = transport.flags;
        peer.transport.hasFlags = transport.hasFlags;
        std::array<std::byte, descriptor::kNetAddrSize> chosen{};
        if (descriptor::normalize_net_addr_ipv4(transport.address, transport.alternate, chosen)
            == descriptor::NetAddrNormalisation::unavailable) {
            continue;
        }
        peer.transport.address = chosen;
        peer.transport.alternate = chosen;
        peer.transport.hasAddress = peer.transport.hasAlternate = true;
    }
}
} // namespace sunrise::server::bap::encrypted::push
