#include "nat_relay_registry.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

#include "../../../core/logging/log.h"
#include "../../../middleware/encoding/byte_order.h"
#include "../../../middleware/gameplay/nat/single_port_frame.h"
#include "../endpoint/gameplay_endpoint.h"

namespace sunrise::server::gameplay::relay {
namespace {
/** One buffered native datagram per unpaired member, at the same bound the carrier wraps. */
constexpr std::size_t kPendingCapacity = middleware::gameplay::single_port::kPayload;
struct Member {
    std::uint32_t connection{};
    state::gameplay::Endpoint endpoint{};
    std::array<std::byte, kPendingCapacity> pending{};
    std::size_t pendingSize{};
    bool bound{};
};
struct Pair {
    std::uint32_t session{};
    std::array<Member, kPairMemberCapacity> members{};
    std::size_t count{};
    bool forwarded{};
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::array<Pair, kPairCapacity> g_pairs{};
std::uint32_t g_nextSession = 1;
std::uint32_t next_session() noexcept {
    if (g_nextSession == (std::numeric_limits<std::uint32_t>::max)()) {
        return 0;
    }
    return g_nextSession++;
}
struct Registration {
    std::uint32_t connection{};
    std::uint32_t identity{};
    state::gameplay::Endpoint endpoint{};
};
std::array<Registration, kClientCapacity> g_clients{};
Registration* registration(std::uint32_t connection) noexcept {
    if (connection) {
        for (auto& client : g_clients) {
            if (client.connection == connection) {
                return &client;
            }
        }
    }
    return nullptr;
}
Pair* find(std::uint32_t first, std::uint32_t second) noexcept {
    for (auto& pair : g_pairs) {
        if (pair.count == 2
            && ((pair.members[0].connection == first && pair.members[1].connection == second)
                || (pair.members[0].connection == second && pair.members[1].connection == first))) {
            return &pair;
        }
    }
    return nullptr;
}
bool same_source(const state::gameplay::Endpoint& first,
                 const state::gameplay::Endpoint& second) noexcept {
    return first.address == second.address && first.port == second.port;
}
void send(const state::gameplay::Endpoint& destination,
          std::span<const std::byte> datagram) noexcept {
    if (!endpoint::send_to(destination, datagram)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=relay stage=forward result=send_failed");
    }
}
} // namespace

std::uint32_t register_client(std::uint32_t connection,
                              const state::gameplay::Endpoint& endpoint) noexcept {
    if (!connection) {
        return 0;
    }
    AcquireSRWLockExclusive(&g_lock);
    auto* client = registration(connection);
    if (!client) {
        for (auto& slot : g_clients) {
            if (slot.connection) {
                continue;
            }
            const auto identity = next_session();
            if (identity) {
                slot = {connection, identity, endpoint};
                client = &slot;
            }
            break;
        }
    }
    const auto identity = client ? client->identity : 0;
    ReleaseSRWLockExclusive(&g_lock);
    return identity;
}

bool pair_clients(std::uint32_t firstConnection, std::uint32_t secondConnection) noexcept {
    if (!firstConnection || !secondConnection || firstConnection == secondConnection) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    const auto* first = registration(firstConnection);
    const auto* second = registration(secondConnection);
    bool paired = find(firstConnection, secondConnection) != nullptr;
    if (!paired && first && second) {
        for (auto& pair : g_pairs) {
            if (pair.count) {
                continue;
            }
            const auto session = next_session();
            if (!session) {
                break;
            }
            pair.session = session;
            pair.count = 2;
            pair.members[0].connection = firstConnection;
            pair.members[0].endpoint = first->endpoint;
            pair.members[0].bound = first->endpoint.address && first->endpoint.port;
            pair.members[1].connection = secondConnection;
            pair.members[1].endpoint = second->endpoint;
            pair.members[1].bound = second->endpoint.address && second->endpoint.port;
            paired = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return paired;
}

std::uint32_t session_id_of(std::uint32_t connection, std::uint32_t peer) noexcept {
    AcquireSRWLockShared(&g_lock);
    std::uint32_t session{};
    if (peer) {
        const auto* pair = find(connection, peer);
        if (pair) {
            session = pair->session;
        }
    } else {
        const auto* client = registration(connection);
        if (client) {
            session = client->identity;
        }
        unsigned pairs{};
        for (const auto& pair : g_pairs) {
            if (pair.count != 2
                || (pair.members[0].connection != connection
                    && pair.members[1].connection != connection)) {
                continue;
            }
            session = pair.session;
            ++pairs;
        }
        if (pairs > 1) {
            session = 0;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return session;
}

std::uint32_t peer_connection_of(std::uint32_t connection) noexcept {
    AcquireSRWLockShared(&g_lock);
    std::uint32_t peer{};
    for (const auto& pair : g_pairs) {
        if (pair.count != 2
            || (pair.members[0].connection != connection
                && pair.members[1].connection != connection)) {
            continue;
        }
        if (peer) {
            peer = 0;
            break;
        }
        peer = pair.members[pair.members[0].connection == connection ? 1 : 0].connection;
    }
    ReleaseSRWLockShared(&g_lock);
    return peer;
}

void release_pair(std::uint32_t first, std::uint32_t second) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (auto* pair = find(first, second)) {
        *pair = {};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void release_client(std::uint32_t connection) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (auto* client = registration(connection)) {
        *client = {};
    }
    for (auto& pair : g_pairs) {
        if (pair.count
            && (pair.members[0].connection == connection
                || pair.members[1].connection == connection)) {
            pair = {};
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}
bool route(const state::gameplay::Endpoint& from, std::span<const std::byte> datagram) noexcept {
    if (datagram.size() <= kFramingHeaderSize || !from.address || !from.port) {
        return false;
    }
    const auto session = middleware::encoding::read_u32_be(datagram.first<kFramingHeaderSize>());
    if (!session) {
        return false;
    }
    const auto relayPort = endpoint::relay_endpoint().port;
    const bool dedicated = relayPort && from.localPort == relayPort;
    state::gameplay::Endpoint destination{}, flushDestination{};
    std::array<std::byte, kPendingCapacity> flush{};
    std::size_t flushSize = 0;
    bool consumed = false;
    bool firstForward = false;
    AcquireSRWLockExclusive(&g_lock);
    for (auto& pair : g_pairs) {
        if (pair.session != session || pair.count != 2) {
            continue;
        }
        Member* sender = nullptr;
        Member* unbound = nullptr;
        for (auto& member : pair.members) {
            if (member.bound && same_source(member.endpoint, from)) {
                sender = &member;
            }
            if (!member.bound && member.endpoint.address == from.address && !unbound) {
                unbound = &member;
            }
        }
        if (!sender && dedicated && unbound) {
            sender = unbound;
            sender->endpoint = from;
            sender->bound = true;
        }
        if (sender) {
            consumed = true;
            auto& peer = pair.members[sender == &pair.members[0] ? 1 : 0];
            if (peer.bound) {
                destination = peer.endpoint;
                firstForward = !pair.forwarded;
                pair.forwarded = true;
            } else if (datagram.size() <= peer.pending.size()) {
                std::copy(datagram.begin(), datagram.end(), peer.pending.begin());
                peer.pendingSize = datagram.size();
            }
            if (sender->pendingSize) {
                flush = sender->pending;
                flushSize = sender->pendingSize;
                flushDestination = sender->endpoint;
                sender->pendingSize = 0;
            }
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (firstForward) {
        std::array<char, 128> line{};
        const auto size = std::snprintf(
            line.data(), line.size(), "ev=relay stage=forward result=paired session=%u", session);
        if (size > 0 && static_cast<std::size_t>(size) < line.size()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(size)});
        }
    }
    if (flushSize) {
        send(flushDestination, std::span(flush).first(flushSize));
    }
    if (destination.port) {
        send(destination, datagram);
    }
    return consumed;
}

void reset() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    for (auto& pair : g_pairs) {
        pair = {};
    }
    g_clients = {};
    ReleaseSRWLockExclusive(&g_lock);
}
} // namespace sunrise::server::gameplay::relay
