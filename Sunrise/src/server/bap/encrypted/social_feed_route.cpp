#include "social_feed_route.h"

#include <algorithm>

#include "../../../core/settings/settings.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "../../../state/account/public_profiles.h"
#include "../../../state/network/peer_routes.h"
#include "../../../state/social/steam_roster.h"
#include "../activity_transport_publication.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted {
namespace {
void append_peer_routes(state::social::feed::Feed& feed, std::uint64_t primarySoid) noexcept {
    namespace descriptor = middleware::gameplay::descriptor;
    namespace profiles = state::account::profiles;
    state::AccountHandle owner{};
    if (!profiles::find(primarySoid, owner)) {
        return;
    }
    const auto character = profiles::selected_character(owner);
    if (!character) {
        return;
    }
    std::array<descriptor::PeerEndpoint, descriptor::kPeerEndpointCount> candidates{};
    auto count = profiles::peer_endpoints(owner, candidates);
    if (count == 0) {
        middleware::bap::activity_message::TransportReport transport{};
        std::array<std::byte, descriptor::kNetAddrSize> chosen{};
        if (published_character_transport_locked(primarySoid, character, transport)) {
            if (descriptor::normalize_net_addr_ipv4(transport.address, transport.alternate, chosen)
                == descriptor::NetAddrNormalisation::unavailable) {
                return;
            }
        } else {
            const auto native = profiles::native_presence(owner);
            if (!native.published || native.characterSoid != character
                || native.descriptorSize != descriptor::kDescriptorSize) {
                return;
            }
            std::copy_n(native.descriptor.begin() + 8, chosen.size(), chosen.begin());
        }
        count = descriptor::net_addr_endpoints(chosen, candidates);
    }
    for (std::size_t j = 0; j < count; ++j) {
        if (feed.routeCount < feed.routes.size()
            && std::find(feed.routes.begin(),
                         feed.routes.begin() + static_cast<std::ptrdiff_t>(feed.routeCount),
                         candidates[j])
                   == feed.routes.begin() + static_cast<std::ptrdiff_t>(feed.routeCount)) {
            feed.routes[feed.routeCount++] = candidates[j];
        }
    }
}
void peer_routes(state::social::feed::Feed& feed) noexcept {
    for (std::size_t i = 0; i < feed.rowCount; ++i) {
        append_peer_routes(feed, feed.rows[i].primarySoid);
    }
}
} // namespace
void service_host_social(std::uint64_t now) noexcept {
    namespace social = state::social;
    static std::uint64_t nextTick{};
    if (now < nextTick) {
        return;
    }
    nextTick = now + 500;
    if (core::settings::role() != core::settings::Role::host
        || !social::initialize_client(state::account_primary_soid(state::kLocalAccount))) {
        return;
    }
    auto& directory = social::session_directory();
    if (directory.link_count(state::kLocalAccount) == 0) {
        return;
    }
    social::feed::Sync sync{};
    social::snapshot_sync(sync);
    social::feed::Feed feed{};
    if (!directory.sync(state::kLocalAccount, sync, feed)) {
        return;
    }
    peer_routes(feed);
    // Friends omit the local player, but the relay must authorize both native endpoints.
    // Resolve the playing host through the same published ownership as every other peer.
    append_peer_routes(feed, state::account_primary_soid(state::kLocalAccount));
    if (social::apply_feed(feed)) {
        static_cast<void>(state::network::peer_routes::replace(
            std::span(feed.routes).first(feed.routeCount), now));
    }
}

bool consume_social_feed(Session& session,
                         Scratch& scratch,
                         const middleware::bap::RequestFrame& request,
                         std::span<std::byte> response,
                         std::size_t& written) noexcept {
    namespace social = state::social;
    written = 0;
    if (!core::settings::hosts_session() || !session.authenticated
        || request.frameType != middleware::bap::FrameType::encrypted
        || request.serviceId != social::feed::kSyncRequest) {
        return false;
    }
    social::feed::Sync sync{};
    if (!social::feed::decode_sync(request.body, sync)) {
        return false;
    }
    auto& staged = scratch.socialDirectory;
    staged = social::session_directory();
    social::feed::Feed feed{};
    if (!staged.sync(session.accountHandle, sync, feed)) {
        return false;
    }
    peer_routes(feed);
    std::array<std::byte, social::feed::max_body_size()> body{};
    std::size_t bodySize{};
    if (!social::feed::encode_feed(feed, body, bodySize)) {
        return false;
    }
    const ServiceRoute route{
        ResponseMode::reply,
        static_cast<middleware::bap::ResponseService>(social::feed::kFeedResponse),
        BodyCodec::empty};
    std::size_t framedSize{};
    if (!reply::encode(scratch,
                       route,
                       request.taskId,
                       session.sessionKey,
                       session.sendNonce,
                       std::span(body).first(bodySize),
                       framedSize)
        || framedSize > response.size()) {
        return false;
    }
    social::session_directory() = staged;
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    middleware::secure_channel::advance_nonce(session.sendNonce);
    written = framedSize;
    return true;
}
} // namespace sunrise::server::bap::encrypted
