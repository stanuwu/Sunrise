#include "proxy_social_feed.h"

#include <Windows.h>

#include "../../../state/account/account_context.h"
#include "../../../state/social/steam_roster.h"
#include "proxy_runtime.h"

namespace sunrise::server::bap::proxy::social_feed {
namespace {
namespace social = state::social;
std::uint32_t connectionId{};
std::uint32_t outstandingTask{};
std::uint64_t nextTick{};
} // namespace
void reset() noexcept {
    connectionId = 0;
    outstandingTask = 0;
    nextTick = 0;
    social::client_disconnected();
    state::network::peer_routes::reset();
}
void abandon(std::uint32_t connection, std::uint32_t task) noexcept {
    if (connectionId == connection && outstandingTask == task) {
        outstandingTask = 0;
        nextTick = 0;
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
    if (!state::network::peer_routes::replace(std::span(feed.routes).first(feed.routeCount),
                                              GetTickCount64())) {
        return false;
    }
    outstandingTask = 0;
    return true;
}
void service(std::uint64_t now) noexcept {
    if (outstandingTask != 0 || now < nextTick) {
        return;
    }
    nextTick = now + 500;
    const auto connection = first_ready_upstream();
    if (connection == 0
        || !social::initialize_client(state::account_primary_soid(state::kLocalAccount))) {
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
        return;
    }
    connectionId = connection;
    outstandingTask = task;
}
} // namespace sunrise::server::bap::proxy::social_feed
