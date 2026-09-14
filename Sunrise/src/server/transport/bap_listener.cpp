#include "bap_listener.h"

#include <WS2tcpip.h>
#include <WinSock2.h>
#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../core/network_service_socket.h"
#include "../../core/settings/settings.h"
#include "../../state/runtime/runtime.h"
#include "../activity/host_runtime.h"
#include "bap_frame_batch.h"
#include "core/threading/data_mutex.h"
#include "internal.h"

namespace sunrise::server::transport {

namespace {

core::threading::DataMutex<Listener> g_listener;

/** Makes one socket nonblocking. @return True when it can no longer block its caller. */
[[nodiscard]] bool make_nonblocking(SOCKET socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) != SOCKET_ERROR;
}

/** @return Index of the first unused peer slot, or the slot count when all are taken. */
[[nodiscard]] std::size_t free_slot(const Listener& listener) noexcept {
    for (std::size_t slot = 0; slot < listener.peers.size(); ++slot) {
        if (listener.peers[slot].socket == INVALID_SOCKET) {
            return slot;
        }
    }
    return listener.peers.size();
}

/**
 * Takes one waiting connection into a free slot and opens its Server session.
 * @param listener Listener holding the acceptor and the peer slots.
 * @param slot Peer slot already checked to be free.
 */
void accept_peer(Listener& listener, std::size_t slot, std::uint64_t now) noexcept {
    sockaddr_in remote{};
    int remoteSize = sizeof remote;
    const SOCKET accepted =
        accept(listener.acceptor, reinterpret_cast<sockaddr*>(&remote), &remoteSize);
    if (accepted == INVALID_SOCKET) {
        return;
    }
    if (!make_nonblocking(accepted)) {
        closesocket(accepted);
        return;
    }
    Peer& peer = listener.peers[slot];
    peer.socket = accepted;
    peer.streamSize = 0;
    peer.outputOffset = 0;
    peer.outputSize = 0;
    peer.connectionId = connection_id(slot);
    peer.remoteAddress = ntohl(remote.sin_addr.s_addr);
    peer.inputDeferred = false;
    peer.authenticated = false;
    peer.acceptedTick = peer.serviceTick = now;
    peer.inputStartedTick = peer.outputProgressTick = now;

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=transport stage=accept result=ok conn=%u", peer.connectionId);
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), length});
    }
    if (!offer(peer, client::network::BapEvent::open, {})) {
        close_peer(peer);
    }
}

/**
 * Reads at most once from one readable peer.
 * @param peer Live peer reported readable.
 */
void receive_peer(Peer& peer) noexcept {
    const std::size_t free = kStreamCapacity - peer.streamSize;
    if (free == 0) {
        return;
    }
    const int received = recv(peer.socket,
                              reinterpret_cast<char*>(peer.stream.data() + peer.streamSize),
                              static_cast<int>(free),
                              0);
    if (received > 0) {
        if (peer.streamSize == 0) {
            peer.inputStartedTick = peer.serviceTick;
        }
        peer.streamSize += static_cast<std::size_t>(received);
        return;
    }
    if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
        close_peer(peer);
    }
}

/**
 * Sends at most once from one peer's committed output.
 * @param peer Live peer.
 * @return True while the peer remains usable.
 */
[[nodiscard]] bool flush_peer(Peer& peer) noexcept {
    if (peer.outputSize == 0) {
        return true;
    }
    const std::size_t remaining = peer.outputSize - peer.outputOffset;
    const core::network::ServiceSocketScope replySocket(peer.socket);
    const int sent = send(peer.socket,
                          reinterpret_cast<const char*>(peer.output.data() + peer.outputOffset),
                          static_cast<int>(remaining),
                          0);
    if (sent > 0) {
        return advance_output(peer, static_cast<std::size_t>(sent));
    }
    return sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
}

/**
 * Services one peer with one read, a bounded frame batch and one due poll.
 * @param peer Live peer.
 * @param readable Read readiness from WSAPoll.
 * @param writable Write readiness from WSAPoll.
 * @param wasPending True when polling began with output.
 * @param pollDue True on a poll tick.
 */
void service_peer(
    Peer& peer, bool readable, bool writable, bool wasPending, bool pollDue) noexcept {
    bool sent = false;
    if (wasPending && writable) {
        sent = true;
        if (!flush_peer(peer)) {
            close_peer(peer);
            return;
        }
    }
    if (peer.socket != INVALID_SOCKET && readable) {
        receive_peer(peer);
    }
    if (peer.socket == INVALID_SOCKET) {
        return;
    }
    if (peer.outputSize == 0 && !drain_frame_batch(peer, drain_stream, flush_peer)) {
        close_peer(peer);
        return;
    }
    if (pollDue && peer.outputSize == 0 && !offer(peer, client::network::BapEvent::poll, {})) {
        close_peer(peer);
        return;
    }
    if (!wasPending && !sent && peer.outputSize != 0 && !flush_peer(peer)) {
        close_peer(peer);
    }
}

/**
 * Starts the nonblocking loopback listener on one port.
 * @param listener Guarded listener to bind.
 * @param port Host-order loopback port. Zero picks an ephemeral port.
 * @return True once the acceptor is bound and listening.
 */
[[nodiscard]] bool start_listener(Listener& listener, std::uint16_t port) noexcept {
    if (listener.active) {
        return true;
    }
    // This DLL initializes before the game touches Winsock, so the listener starts it itself.
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        return false;
    }
    listener.winsockOwned = true;
    listener.acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener.acceptor == INVALID_SOCKET) {
        WSACleanup();
        listener.winsockOwned = false;
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    std::uint32_t bindAddress = INADDR_LOOPBACK;
    if (core::settings::hosts_session()) {
        bindAddress = 0;
        for (const auto octet : core::settings::get().server.bapBind) {
            bindAddress = (bindAddress << 8U) | octet;
        }
    }
    address.sin_addr.s_addr = htonl(bindAddress);
    BOOL exclusive = TRUE;
    if (setsockopt(listener.acceptor,
                   SOL_SOCKET,
                   SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive),
                   sizeof exclusive)
            == SOCKET_ERROR
        || !make_nonblocking(listener.acceptor)
        || bind(listener.acceptor, reinterpret_cast<const sockaddr*>(&address), sizeof address)
               == SOCKET_ERROR
        || listen(listener.acceptor, static_cast<int>(listener.peers.size())) == SOCKET_ERROR) {
        closesocket(listener.acceptor);
        listener.acceptor = INVALID_SOCKET;
        WSACleanup();
        listener.winsockOwned = false;
        return false;
    }
    listener.active = true;
    int addressSize = sizeof address;
    if (getsockname(listener.acceptor, reinterpret_cast<sockaddr*>(&address), &addressSize)
        == SOCKET_ERROR) {
        closesocket(listener.acceptor);
        listener.acceptor = INVALID_SOCKET;
        listener.active = false;
        WSACleanup();
        listener.winsockOwned = false;
        return false;
    }
    port = ntohs(address.sin_port);
    state::publish_bap_port(port);
    listener.nextPollTick = 0;
    std::array<char, 64> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=transport stage=listen result=ok port=%u",
                                      static_cast<unsigned>(port));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

} // namespace

/** Starts the nonblocking loopback listener on one port. */
bool initialize_on_port(std::uint16_t port) noexcept {
    return g_listener.lock([port](Listener& listener) { return start_listener(listener, port); });
}

/** Starts the nonblocking listener on the configured BAP port. */
bool initialize() noexcept {
    return initialize_on_port(core::settings::get().server.bapPort);
}

/** Runs one bounded listener slice on the caller thread. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept {
    const bool ran = g_listener.try_lock([now](Listener& listener) {
        if (!listener.active) {
            return;
        }

        for (auto& peer : listener.peers) {
            if (peer.socket == INVALID_SOCKET) {
                continue;
            }
            peer.serviceTick = now;
            if (expired(peer, now)) {
                close_peer(peer);
            }
        }

        // Winsock fd_set defaults to 64 entries; each player owns several sockets.
        // Poll every active slot so later players cannot silently lose read/write service.
        std::array<WSAPOLLFD, client::network::kBapConnectionCount + 1> poll{};
        for (auto& entry : poll) {
            entry.fd = INVALID_SOCKET;
        }
        std::array<bool, client::network::kBapConnectionCount> wasPending{};
        const std::size_t accepting = free_slot(listener);
        // With no free slot the acceptor is left out of the set, so a connect waits in the backlog
        // with no handshake and no other symptom. Report the edge.
        const bool full = accepting == listener.peers.size();
        if (full != listener.slotsFull) {
            listener.slotsFull = full;
            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=transport stage=accept result=%s slots=%zu",
                                              full ? "full" : "free",
                                              listener.peers.size());
            if (written > 0) {
                core::log::write(core::log::Channel::server,
                                 full ? core::log::Level::warn : core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        if (!full) {
            poll[0] = {listener.acceptor, POLLRDNORM, 0};
        }
        for (std::size_t slot = 0; slot < listener.peers.size(); ++slot) {
            const Peer& peer = listener.peers[slot];
            if (peer.socket == INVALID_SOCKET) {
                continue;
            }
            auto& entry = poll[slot + 1];
            entry.fd = peer.socket;
            if (peer.streamSize < kStreamCapacity) {
                entry.events |= POLLRDNORM;
            }
            if (peer.outputSize != 0) {
                entry.events |= POLLWRNORM;
                wasPending[slot] = true;
            }
        }
        if (WSAPoll(poll.data(), static_cast<ULONG>(poll.size()), 0) == SOCKET_ERROR) {
            return;
        }

        const bool timedPoll = listener.nextPollTick == 0 || now >= listener.nextPollTick;
        if (timedPoll) {
            listener.nextPollTick = now + static_cast<std::uint64_t>(kServiceIntervalMs);
        }
        // The poll is what lets a committed answer out. Holding one for the rest of the
        // interval costs every queued mission action a full interval of its own.
        const bool pollDue = timedPoll || activity::host::any_output_pending();
        if (!full && (poll[0].revents & POLLRDNORM)) {
            accept_peer(listener, accepting, now);
        }
        for (std::size_t slot = 0; slot < listener.peers.size(); ++slot) {
            Peer& peer = listener.peers[slot];
            // A socket accepted above did not participate in this poll. Its previously
            // unused entry may carry POLLNVAL, so service it only after the next poll.
            if (peer.socket != INVALID_SOCKET && poll[slot + 1].fd == peer.socket) {
                const auto events = poll[slot + 1].revents;
                if (events & (POLLERR | POLLNVAL)) {
                    close_peer(peer);
                    continue;
                }
                service_peer(peer,
                             (events & (POLLRDNORM | POLLHUP)) != 0,
                             (events & POLLWRNORM) != 0,
                             wasPending[slot],
                             pollDue);
            }
        }
    });
    if (!ran) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=transport stage=service result=busy");
    }
}

/** Closes every socket owned by the listener. */
void shutdown() noexcept {
    g_listener.lock([](Listener& listener) {
        if (!listener.active) {
            return;
        }
        listener.active = false;
        if (listener.acceptor != INVALID_SOCKET) {
            closesocket(listener.acceptor);
            listener.acceptor = INVALID_SOCKET;
        }
        for (Peer& peer : listener.peers) {
            close_peer(peer);
        }
        listener.nextPollTick = 0;
        listener.slotsFull = false;
        if (listener.winsockOwned) {
            WSACleanup();
            listener.winsockOwned = false;
        }
    });
}

} // namespace sunrise::server::transport
