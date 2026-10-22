#include "proxy_social_feed.h"

#include <array>

#include "../../../state/account/account_context.h"
#include "../../../state/social/steam_roster.h"
#include "proxy_internal.h"
#include "proxy_runtime.h"

namespace sunrise::server::bap::proxy::social_feed {
namespace {
namespace social = state::social;
/** Service id and sequence: the smallest inner header a server notification is encoded with. */
constexpr std::size_t kNotificationHeaderSize = 6;
std::uint32_t connectionId{};
std::uint32_t outstandingTask{};
/** Set once a request has been accepted on `connectionId`; a new link must register again. */
bool registered{};
/** Publication the last accepted feed was built from; the guest asks again past this value. */
std::uint64_t sentPublication{};
} // namespace
void reset() noexcept {
    connectionId = 0;
    outstandingTask = 0;
    registered = false;
    sentPublication = 0;
    social::client_disconnected();
    state::network::peer_routes::reset();
}
void abandon(std::uint32_t connection, std::uint32_t task) noexcept {
    if (connectionId == connection && outstandingTask == task) {
        outstandingTask = 0;
    }
}
bool acknowledge(std::uint32_t connection,
                 std::uint32_t task,
                 std::span<const std::byte> body) noexcept {
    if (connectionId != connection || outstandingTask == 0 || outstandingTask != task) {
        return false;
    }
    social::feed::Feed feed{};
    if (!social::feed::decode_feed(body, feed) || !social::apply_feed(feed)) {
        return false;
    }
    if (!state::network::peer_routes::replace(std::span(feed.routes).first(feed.routeCount))) {
        return false;
    }
    // The answer names the publication it was built from, which is what this client has now been
    // told. Taking the client's own high-water instead would swallow a notice that raced the feed.
    sentPublication = feed.publication;
    outstandingTask = 0;
    return true;
}
bool notify(std::uint32_t connection, std::span<const std::byte> payload) noexcept {
    if (connection != connectionId || connectionId == 0) {
        // A late notice on a replaced registration is consumed and changes nothing.
        return true;
    }
    if (payload.size() != kNotificationHeaderSize + social::feed::kNoticeBodySize) {
        fail_connection(connection, "social_notice_size");
        return false;
    }
    std::uint64_t publication{};
    if (!social::feed::decode_notice(payload.subspan(kNotificationHeaderSize), publication)) {
        fail_connection(connection, "social_notice_decode");
        return false;
    }
    social::note_publication(publication);
    return true;
}
void connection_closed(std::uint32_t connection) noexcept {
    if (connection == 0 || connection != connectionId) {
        return;
    }
    // A route is an authorisation and must fail closed; the roster rows are a cache and survive
    // until the re-registration's feed replaces them or the last upstream link goes away.
    connectionId = 0;
    outstandingTask = 0;
    registered = false;
    sentPublication = 0;
    state::network::peer_routes::reset();
}
void service() noexcept {
    if (outstandingTask != 0) {
        // One social correlation at a time; its answer is what advances every term below.
        return;
    }
    const auto connection = first_ready_upstream();
    if (connection == 0
        || !social::initialize_client(state::account_primary_soid(state::kLocalAccount))) {
        return;
    }
    if (connection != connectionId) {
        registered = false;
        social::reset_publication();
    }
    // Level-triggered: registration, real local work and the host's publication are all read from
    // current state, so a refusal below leaves the condition true and the next pass retries it.
    if (registered && !social::pending_local_work()
        && social::known_publication() == sentPublication) {
        return;
    }
    social::feed::Sync sync{};
    social::snapshot_sync(sync);
    std::array<std::byte, social::feed::max_body_size()> body{};
    std::size_t bodySize{};
    if (!social::feed::encode_sync(sync, body, bodySize)) {
        return;
    }
    std::uint32_t task{};
    if (!send_upstream_request(connection,
                               social::feed::kSyncRequest,
                               social::feed::kFeedResponse,
                               std::span(body).first(bodySize),
                               task)) {
        // Nothing committed, so the unchanged condition simply retries on the next service pass.
        return;
    }
    connectionId = connection;
    outstandingTask = task;
    registered = true;
}
} // namespace sunrise::server::bap::proxy::social_feed
