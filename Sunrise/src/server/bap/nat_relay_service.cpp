#include "nat_relay_service.h"

#include <algorithm>

#include "../../middleware/gameplay/descriptor/net_addr.h"
#include "../../state/activity/member_presence.h"
#include "../gameplay/endpoint/gameplay_endpoint.h"
#include "../gameplay/group/group_host_admission.h"
#include "../gameplay/relay/nat_relay_registry.h"
#include "internal.h"

namespace sunrise::server::bap::nat_relay {
namespace {
namespace registry = gameplay::relay;
using Address = middleware::bap::nat_relay::SecureAddress;
Session* link(std::uint32_t id) noexcept {
    for (auto& session : sessions()) {
        if (id && session.id == id && session.authenticated) {
            return &session;
        }
    }
    return nullptr;
}
std::uint64_t account(const Session& session) noexcept {
    return state::account_primary_soid(session.accountHandle);
}
bool same_core(const Address& first, const Address& second) noexcept {
    // A shared LAN address is not a peer identity across different NATs. Prefer the
    // native public endpoint, keeping the local endpoint as a legacy fallback. Both are
    // candidates of the NetAddr's own list: four address bytes then a two-byte port.
    namespace descriptor = middleware::gameplay::descriptor;
    constexpr auto kPublic = static_cast<std::ptrdiff_t>(descriptor::kPublicEndpointOffset);
    constexpr auto kStride = static_cast<std::ptrdiff_t>(descriptor::kPeerEndpointStride);
    constexpr std::ptrdiff_t kAddressBytes = 4;
    const auto hasPublic = [](const Address& address) {
        return std::any_of(address.begin() + kPublic,
                           address.begin() + kPublic + kAddressBytes,
                           [](std::byte value) { return value != std::byte{}; })
               && (address[kPublic + kAddressBytes] != std::byte{}
                   || address[kPublic + kAddressBytes + 1] != std::byte{});
    };
    if (hasPublic(first) || hasPublic(second)) {
        return hasPublic(first) && hasPublic(second)
               && std::equal(first.begin() + kPublic,
                             first.begin() + kPublic + kStride,
                             second.begin() + kPublic);
    }
    return std::equal(first.begin(), first.begin() + kStride, second.begin());
}
bool native_address(const Session& session) noexcept {
    if (!session.id || !session.authenticated || !session.activity.bindingGeneration
        || session.activity.role == ActivityClientRole::none || !session.activityMemberKey
        || session.activityJoinGeneration != session.activity.bindingGeneration
        || !session.activityTransport.hasAddress) {
        return false;
    }
    const auto& address = session.activityTransport.address;
    // Neither the first local candidate nor the public mapping carries anything.
    namespace descriptor = middleware::gameplay::descriptor;
    constexpr auto kPublic = static_cast<std::ptrdiff_t>(descriptor::kPublicEndpointOffset);
    constexpr auto kStride = static_cast<std::ptrdiff_t>(descriptor::kPeerEndpointStride);
    if (std::all_of(address.begin(),
                    address.begin() + kStride,
                    [](std::byte b) { return b == std::byte{}; })
        && std::all_of(address.begin() + kPublic,
                       address.begin() + kPublic + kStride,
                       [](std::byte b) { return b == std::byte{}; })) {
        return false;
    }
    // This is the native typed publication, never the reference's account-text fallback.
    // An address octet that happens to be an ASCII letter is still a valid binary address.
    state::activity::membership::ClientPlacement placement{};
    return state::activity::presence::placement(
        session.activity.session, account(session), session.activityCharacterSoid, placement);
}
Publication publication(const Session& session) noexcept {
    return {session.id, session.activity.bindingGeneration, session.activityTransport.address};
}
bool current(const Publication& value) noexcept {
    const auto* session = link(value.connection);
    return session && session->activity.bindingGeneration == value.generation
           && native_address(*session) && session->activityTransport.address == value.address;
}
Session* registration(Session& preferred) noexcept {
    if (preferred.relay.registered) {
        return &preferred;
    }
    Session* found = nullptr;
    for (auto& candidate : sessions()) {
        if (!candidate.id || !candidate.authenticated || !candidate.relay.registered
            || account(candidate) != account(preferred)) {
            continue;
        }
        if (found) {
            return nullptr;
        }
        found = &candidate;
    }
    return found;
}
Session* own_publication(Session& source) noexcept {
    if (native_address(source)) {
        return &source;
    }
    Session* found = nullptr;
    for (auto& candidate : sessions()) {
        if (account(candidate) != account(source) || !native_address(candidate)) {
            continue;
        }
        if (found
            && (found->activityTransport.address != candidate.activityTransport.address
                || found->activityCharacterSoid != candidate.activityCharacterSoid)) {
            return nullptr;
        }
        if (!found || candidate.activity.session.sessionId > found->activity.session.sessionId) {
            found = &candidate;
        }
    }
    return found;
}
Session*
peer_publication(const Session& source, const Address& wanted, const Session* own) noexcept {
    for (bool exact : {true, false}) {
        Session* found = nullptr;
        for (auto& candidate : sessions()) {
            if (!native_address(candidate) || !account(candidate)
                || account(candidate) == account(source)
                || !gameplay::group::admission::accounts_coresident(account(source),
                                                                    account(candidate))) {
                continue;
            }
            const auto& address = candidate.activityTransport.address;
            if (exact ? address != wanted : !same_core(address, wanted)) {
                continue;
            }
            if (!exact && own && same_core(address, own->activityTransport.address)) {
                continue;
            }
            if (found
                && (account(*found) != account(candidate)
                    || found->activityCharacterSoid != candidate.activityCharacterSoid
                    || found->activityTransport.address != address)) {
                return nullptr;
            }
            if (!found
                || candidate.activity.session.sessionId > found->activity.session.sessionId) {
                found = &candidate;
            }
        }
        if (found) {
            return found;
        }
    }
    return nullptr;
}
PairState* pair_state(Session& session, std::uint32_t peer) noexcept {
    for (auto& pair : session.relay.pairs) {
        if (pair.pairSession && pair.peerRegistration == peer) {
            return &pair;
        }
    }
    return nullptr;
}
PairState* available_pair(Session& session, std::uint32_t peer) noexcept {
    if (auto* pair = pair_state(session, peer)) {
        return pair;
    }
    for (auto& pair : session.relay.pairs) {
        if (!pair.pairSession) {
            return &pair;
        }
    }
    return nullptr;
}
void retire_pair(Session& session, PairState& pair) noexcept {
    if (auto* peer = link(pair.peerRegistration)) {
        if (auto* other = pair_state(*peer, session.id)) {
            *other = {};
        }
    }
    registry::release_pair(session.id, pair.peerRegistration);
    pair = {};
}
void arm(PairState& state,
         const Session& own,
         Session& peer,
         const Session& remote,
         std::uint32_t pair) noexcept {
    if (state.pairSession == pair) {
        return;
    }
    state = {};
    state.pairSession = pair;
    state.peerRegistration = peer.id;
    state.local = publication(own);
    state.remote = publication(remote);
    state.notificationPending = true;
}
bool valid_pair(const Session& session, const PairState& relay) noexcept {
    auto* peer = link(relay.peerRegistration);
    const auto* other = peer ? pair_state(*peer, session.id) : nullptr;
    return peer && peer->relay.registered && other && other->pairSession == relay.pairSession
           && registry::session_id_of(session.id, peer->id) == relay.pairSession
           && current(relay.local) && current(relay.remote)
           && gameplay::group::admission::accounts_coresident(account(session), account(*peer));
}
} // namespace

void register_client(Session& session) noexcept {
    if (!session.authenticated || !account(session) || !gameplay::endpoint::relay_endpoint().port) {
        return;
    }
    if (!session.remoteAddress) {
        return;
    }
    session.relay.registered = registry::register_client(session.id, {session.remoteAddress}) != 0;
}
void initiate(Session& session,
              const middleware::bap::nat_relay::InitiateRelayConnection& request) noexcept {
    if (!session.authenticated || !account(session)) {
        return;
    }
    service();
    PendingRequest* pending{};
    for (auto& entry : session.relay.requests) {
        if (entry.initiatePending && entry.requestedPeer == request.peerAddress) {
            pending = &entry;
            break;
        }
        if (!entry.initiatePending && !pending) {
            pending = &entry;
        }
    }
    if (!pending) {
        return;
    }
    pending->requestedPeer = request.peerAddress;
    pending->initiateGeneration = session.activity.bindingGeneration;
    pending->initiatePending = true;
    service();
}
void release(Session& session) noexcept {
    for (auto& pair : session.relay.pairs) {
        if (pair.pairSession) {
            retire_pair(session, pair);
        }
    }
    registry::release_client(session.id);
    session.relay = {};
}
void service() noexcept {
    for (auto& session : sessions()) {
        service(session);
    }
}
void service(Session& source) noexcept {
    for (auto& pair : source.relay.pairs) {
        if (pair.pairSession && !valid_pair(source, pair)) {
            retire_pair(source, pair);
        }
    }
    if (!gameplay::endpoint::relay_endpoint().port) {
        return;
    }
    for (auto& request : source.relay.requests) {
        if (!request.initiatePending) {
            continue;
        }
        if (!source.id || !source.authenticated
            || request.initiateGeneration != source.activity.bindingGeneration) {
            request = {};
            continue;
        }
        auto* sender = registration(source);
        auto* own = own_publication(source);
        auto* remote = peer_publication(source, request.requestedPeer, own);
        auto* peer = remote ? registration(*remote) : nullptr;
        if (!sender || !own || !remote || !peer || sender == peer) {
            continue;
        }
        auto* localState = available_pair(*sender, peer->id);
        auto* remoteState = available_pair(*peer, sender->id);
        if (!localState || !remoteState || !registry::pair_clients(sender->id, peer->id)) {
            continue;
        }
        const auto pair = registry::session_id_of(sender->id, peer->id);
        arm(*localState, *own, *peer, *remote, pair);
        arm(*remoteState, *remote, *sender, *own, pair);
        request = {};
    }
}
void connectivity_failure(Session& session) noexcept {
    service();
    auto* reporter = registration(session);
    if (!reporter) {
        return;
    }
    for (auto& pair : reporter->relay.pairs) {
        if (!pair.pairSession || !valid_pair(*reporter, pair)) {
            continue;
        }
        auto* peer = link(pair.peerRegistration);
        auto* other = peer ? pair_state(*peer, reporter->id) : nullptr;
        if (!other || pair.failureRepushSpent || other->failureRepushSpent) {
            continue;
        }
        pair.failureRepushSpent = other->failureRepushSpent = true;
        pair.notificationPending = other->notificationPending = true;
    }
}
} // namespace sunrise::server::bap::nat_relay
