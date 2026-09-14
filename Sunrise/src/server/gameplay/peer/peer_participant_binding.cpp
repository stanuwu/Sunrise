#include <algorithm>

#include "../../../middleware/encoding/bit_raw.h"
#include "../../../middleware/gameplay/group/session_messages.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../group/group_host_sessions.h"
#include "peer_transport_internal.h"

namespace sunrise::server::gameplay::peer {
void bind_participant(const state::gameplay::Endpoint& from,
                      const state::gameplay::entity_identity::Source& ingress) noexcept {
    namespace gp = state::gameplay;
    namespace wire = middleware::gameplay;
    if (!from.localPort) {
        return;
    }
    struct Sibling {
        gp::Endpoint endpoint{};
        std::uint64_t peerGeneration{}, channelGeneration{};
        std::array<std::uint64_t, gp::kSessionsPerLink> sessions{};
    };
    std::array<Sibling, gp::kAssociationCapacity> siblings{};
    std::size_t count{};
    AcquireSRWLockShared(&g_lock);
    for (const auto& source : g_peers) {
        if (source.stage != gp::PeerStage::absent && source.endpoint.address == from.address
            && source.endpoint.port == from.port && source.endpoint.localPort != from.localPort) {
            siblings[count++] = {
                source.endpoint, source.peerGeneration, source.channelGeneration, source.sessions};
        }
    }
    ReleaseSRWLockShared(&g_lock);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& sibling = siblings[index];
        for (const auto held : sibling.sessions) {
            group::HostSessionBinding host{};
            if (!held || !group::host_session_for_group(held, host) || host.port != from.localPort
                || !group::retain_host_session(host.generation)) {
                continue;
            }
            // The native peer establishes a second channel at its activity-host port. Reuse
            // only its own owner link, and only the group that this exact host port advertises.
            std::array<std::byte, 8> body{};
            middleware::encoding::bits::Writer writer(body);
            std::size_t size{};
            if (!middleware::encoding::bits::write_raw_u64(writer, held) || !writer.finish(size)) {
                group::release_host_session(host.generation);
                continue;
            }
            AcquireSRWLockExclusive(&g_lock);
            auto* target = find_locked(from);
            const auto* source = find_locked(sibling.endpoint);
            if (target && source && target->stage == gp::PeerStage::connected
                && target->establishQueued && target->peerGeneration == ingress.peerGeneration
                && target->channelGeneration == ingress.channelGeneration
                && source->peerGeneration == sibling.peerGeneration
                && source->channelGeneration == sibling.channelGeneration
                && carries_locked(*source, held) && !carries_locked(*target, held)) {
                auto slot = std::find(target->sessions.begin(), target->sessions.end(), 0);
                if (slot != target->sessions.end()
                    && wire::peer::enqueue_message(
                        target->outbound,
                        static_cast<std::uint8_t>(wire::group::SessionMessageId::peerEstablish),
                        wire::group::kPeerEstablishSize,
                        std::span(body).first(size),
                        writer.bit_count())) {
                    *slot = held;
                    target->acknowledgementOwed = true;
                }
            }
            ReleaseSRWLockExclusive(&g_lock);
            group::release_host_session(host.generation);
        }
    }
}
} // namespace sunrise::server::gameplay::peer
