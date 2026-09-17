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
            std::copy_n(native.descriptor.begin() + state::social::kNativeJoinAddressOffset,
                        chosen.size(),
                        chosen.begin());
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
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}
/** Stamp of the mirror the host has already applied to itself. */
state::social::Stamp g_hostStamp{};
/** Delivery token shared by feeds and notices; feeds also use it as their registration serial. */
std::uint64_t g_nextSocialSerial{1};
} // namespace
void service_host_social() noexcept {
    namespace social = state::social;
    namespace profiles = state::account::profiles;
    if (core::settings::role() != core::settings::Role::host
        || !social::initialize_client(state::account_primary_soid(state::kLocalAccount))) {
        return;
    }
    auto& directory = social::session_directory();
    if (directory.link_count(state::kLocalAccount) == 0) {
        return;
    }
    // Sample the route generation before deriving from it: a change during composition leaves the
    // stamp behind the routes, which asks again, where the reverse would never ask.
    const auto routeGeneration = profiles::public_generation();
    if (directory.stamp(state::kLocalAccount, routeGeneration) == g_hostStamp
        && !social::pending_local_work()) {
        return;
    }
    social::feed::Sync sync{};
    social::snapshot_sync(sync);
    if (!directory.apply(state::kLocalAccount, sync)) {
        return;
    }
    social::feed::Feed feed{};
    directory.publish(state::kLocalAccount, feed);
    peer_routes(feed);
    // Friends omit the local player, but the relay must authorize both native endpoints.
    // Resolve the playing host through the same published ownership as every other peer.
    append_peer_routes(feed, state::account_primary_soid(state::kLocalAccount));
    if (social::apply_feed(feed)) {
        static_cast<void>(
            state::network::peer_routes::replace(std::span(feed.routes).first(feed.routeCount)));
    }
    // Read after the apply: the host's own request advances its own publication, so the pre-apply
    // stamp would never match and this mirror would rebuild on every service pass.
    g_hostStamp = directory.stamp(state::kLocalAccount, routeGeneration);
}

void reset_host_social() noexcept {
    g_hostStamp = {};
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
    const auto routeGeneration = state::account::profiles::public_generation();
    auto& staged = scratch.socialDirectory;
    staged = social::session_directory();
    if (!staged.apply(session.accountHandle, sync)) {
        return false;
    }
    social::feed::Feed feed{};
    staged.publish(session.accountHandle, feed);
    peer_routes(feed);
    // A mailbox counter cannot represent directory, route or lobby changes. Every accepted
    // delivery gets a fresh token; the full stamp below decides when a notice is owed.
    feed.publication = g_nextSocialSerial;
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
    // This request registers the connection that owns the account's social delivery; a later one
    // on another link simply replaces it, and late frames on the old link match nothing. The
    // identity is allocated here so a refused reply above spends none of it.
    const auto serial = g_nextSocialSerial++;
    staged.delivery(session.accountHandle, session.id, serial);
    social::session_directory() = staged;
    session.socialSerial = serial;
    // Answering the request discharges the notice for exactly what the guest has now been told.
    session.socialSentStamp =
        social::session_directory().stamp(session.accountHandle, routeGeneration);
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    middleware::secure_channel::advance_nonce(session.sendNonce);
    written = framedSize;
    return true;
}

bool consume_social_notice(Session& session,
                           Scratch& scratch,
                           std::span<std::byte> response,
                           std::size_t& written,
                           bool& touchesScratch) noexcept {
    namespace social = state::social;
    written = 0;
    const auto& directory = social::session_directory();
    if (!core::settings::hosts_session() || !session.authenticated
        || !directory.delivers(session.accountHandle, session.id, session.socialSerial)) {
        return false;
    }
    const auto stamp =
        directory.stamp(session.accountHandle, state::account::profiles::public_generation());
    if (stamp == session.socialSentStamp) {
        return false;
    }
    // A change in any stamp component owes a fresh delivery token, without building the feed.
    std::array<std::byte, social::feed::kNoticeBodySize> body{};
    std::size_t bodySize = 0, payloadSize = 0, sealedSize = 0, frameSize = 0;
    touchesScratch = true;
    const bool encoded =
        social::feed::encode_notice(g_nextSocialSerial, body, bodySize)
        && middleware::bap::encode_notification_payload(
            middleware::bap::NotificationService::socialPublication,
            0,
            std::span(body).first(bodySize),
            scratch.sealed,
            payloadSize)
        && middleware::secure_channel::seal_frame(session.sessionKey,
                                                  session.sendNonce,
                                                  std::span(scratch.sealed).first(payloadSize),
                                                  scratch.plaintext,
                                                  sealedSize)
        && middleware::bap::encode_frame(middleware::bap::FrameType::encrypted,
                                         std::span(scratch.plaintext).first(sealedSize),
                                         scratch.framed,
                                         frameSize)
        && frameSize <= response.size();
    if (encoded) {
        std::copy_n(scratch.framed.begin(), frameSize, response.begin());
        written = frameSize;
        middleware::secure_channel::advance_nonce(session.sendNonce);
        // Committed only once the complete frame has reached the caller's buffer, so an output
        // refusal re-attempts on the next poll with no state change and no spent nonce.
        session.socialSentStamp = stamp;
        ++g_nextSocialSerial;
    }
    clear_prefix(scratch.sealed, payloadSize);
    clear_prefix(scratch.plaintext, payloadSize + middleware::secure_channel::kFrameTagSize);
    return encoded;
}
} // namespace sunrise::server::bap::encrypted
