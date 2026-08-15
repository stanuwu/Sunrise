#include "bap_listener.h"

#include <WS2tcpip.h>
#include <WinSock2.h>
#include <array>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "internal.h"

namespace sunrise::server::transport {

Listener g_listener;

namespace {

SRWLOCK g_listenerLock{SRWLOCK_INIT};

/** Makes one socket nonblocking. @return True when it can no longer block its caller. */
[[nodiscard]] bool make_nonblocking(SOCKET socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) != SOCKET_ERROR;
}

/** @return Index of the first unused peer slot, or the slot count when all are taken. */
[[nodiscard]] std::size_t free_slot() noexcept {
    for (std::size_t slot = 0; slot < g_listener.peers.size(); ++slot) {
        if (g_listener.peers[slot].socket == INVALID_SOCKET) {
            return slot;
        }
    }
    return g_listener.peers.size();
}

/**
 * Takes one waiting connection into a free slot and opens its Server session.
 * @param slot Peer slot already checked to be free.
 */
void accept_peer(std::size_t slot) noexcept {
    const SOCKET accepted = accept(g_listener.acceptor, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
        return;
    }
    if (!make_nonblocking(accepted)) {
        closesocket(accepted);
        return;
    }
    Peer& peer = g_listener.peers[slot];
    peer.socket = accepted;
    peer.streamSize = 0;
    peer.outputOffset = 0;
    peer.outputSize = 0;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=transport stage=accept result=ok conn=%u",
                                      connection_id(slot));
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1;
        core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), length});
    }
    if (!offer(slot, client::network::BapEvent::open, {})) {
        close_peer(slot);
    }
}

/**
 * Reads at most once from one readable peer.
 * @param slot Live peer slot reported readable.
 */
void receive_peer(std::size_t slot) noexcept {
    Peer& peer = g_listener.peers[slot];
    const std::size_t free = kStreamCapacity - peer.streamSize;
    if (free == 0) {
        return;
    }
    const int received = recv(peer.socket,
                              reinterpret_cast<char*>(peer.stream.data() + peer.streamSize),
                              static_cast<int>(free),
                              0);
    if (received > 0) {
        peer.streamSize += static_cast<std::size_t>(received);
        return;
    }
    if (received == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
        close_peer(slot);
    }
}

/**
 * Sends at most once from one peer's committed output.
 * @param slot Live peer slot.
 * @return True while the peer remains usable.
 */
[[nodiscard]] bool flush_peer(std::size_t slot) noexcept {
    Peer& peer = g_listener.peers[slot];
    if (peer.outputSize == 0) {
        return true;
    }
    const std::size_t remaining = peer.outputSize - peer.outputOffset;
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
 * Services one peer with one read, frame, write and due-poll budget.
 * @param readable Ready-read set from select.
 * @param writable Ready-write set from select.
 * @param wasPending True when select saw output.
 * @param pollDue True on a poll tick.
 */
void service_peer(
    std::size_t slot, fd_set& readable, fd_set& writable, bool wasPending, bool pollDue) noexcept {
    Peer& peer = g_listener.peers[slot];
    bool sent = false;
    if (wasPending && FD_ISSET(peer.socket, &writable)) {
        sent = true;
        if (!flush_peer(slot)) {
            close_peer(slot);
            return;
        }
    }
    if (peer.socket != INVALID_SOCKET && FD_ISSET(peer.socket, &readable)) {
        receive_peer(slot);
    }
    if (peer.socket == INVALID_SOCKET) {
        return;
    }
    if (peer.outputSize == 0 && !drain_stream(slot)) {
        close_peer(slot);
        return;
    }
    if (pollDue && peer.outputSize == 0 && !offer(slot, client::network::BapEvent::poll, {})) {
        close_peer(slot);
        return;
    }
    if (!wasPending && !sent && peer.outputSize != 0 && !flush_peer(slot)) {
        close_peer(slot);
    }
}

} // namespace

/** Starts the nonblocking loopback listener on one port. */
bool initialize_on_port(std::uint16_t port) noexcept {
    AcquireSRWLockExclusive(&g_listenerLock);
    if (g_listener.active) {
        ReleaseSRWLockExclusive(&g_listenerLock);
        return true;
    }
    // This DLL initializes before the game touches Winsock, so the listener starts it itself.
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        ReleaseSRWLockExclusive(&g_listenerLock);
        return false;
    }
    g_listener.winsockOwned = true;
    g_listener.acceptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listener.acceptor == INVALID_SOCKET) {
        WSACleanup();
        g_listener.winsockOwned = false;
        ReleaseSRWLockExclusive(&g_listenerLock);
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL reuse = TRUE;
    (void)setsockopt(g_listener.acceptor,
                     SOL_SOCKET,
                     SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse),
                     sizeof reuse);
    if (!make_nonblocking(g_listener.acceptor)
        || bind(g_listener.acceptor, reinterpret_cast<const sockaddr*>(&address), sizeof address)
               == SOCKET_ERROR
        || listen(g_listener.acceptor, static_cast<int>(g_listener.peers.size())) == SOCKET_ERROR) {
        closesocket(g_listener.acceptor);
        g_listener.acceptor = INVALID_SOCKET;
        WSACleanup();
        g_listener.winsockOwned = false;
        ReleaseSRWLockExclusive(&g_listenerLock);
        return false;
    }
    g_listener.active = true;
    g_listener.nextPollTick = 0;
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
    ReleaseSRWLockExclusive(&g_listenerLock);
    return true;
}

/** Starts the nonblocking listener on the configured BAP port. */
bool initialize() noexcept {
    return initialize_on_port(core::settings::get().server.bapPort);
}

/** Runs one bounded listener slice on the caller thread. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_listenerLock)) {
        return;
    }
    if (!g_listener.active) {
        ReleaseSRWLockExclusive(&g_listenerLock);
        return;
    }

    fd_set readable;
    fd_set writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    std::array<bool, client::network::kBapConnectionCount> wasPending{};
    const std::size_t accepting = free_slot();
    if (accepting != g_listener.peers.size()) {
        FD_SET(g_listener.acceptor, &readable);
    }
    for (std::size_t slot = 0; slot < g_listener.peers.size(); ++slot) {
        const Peer& peer = g_listener.peers[slot];
        if (peer.socket == INVALID_SOCKET) {
            continue;
        }
        if (peer.streamSize < kStreamCapacity) {
            FD_SET(peer.socket, &readable);
        }
        if (peer.outputSize != 0) {
            FD_SET(peer.socket, &writable);
            wasPending[slot] = true;
        }
    }
    timeval timeout{};
    if (select(0, &readable, &writable, nullptr, &timeout) == SOCKET_ERROR) {
        ReleaseSRWLockExclusive(&g_listenerLock);
        return;
    }

    const bool pollDue = g_listener.nextPollTick == 0 || now >= g_listener.nextPollTick;
    if (pollDue) {
        g_listener.nextPollTick = now + static_cast<std::uint64_t>(kServiceIntervalMs);
    }
    if (accepting != g_listener.peers.size() && FD_ISSET(g_listener.acceptor, &readable)) {
        accept_peer(accepting);
    }
    for (std::size_t slot = 0; slot < g_listener.peers.size(); ++slot) {
        if (g_listener.peers[slot].socket != INVALID_SOCKET) {
            service_peer(slot, readable, writable, wasPending[slot], pollDue);
        }
    }
    ReleaseSRWLockExclusive(&g_listenerLock);
}

/** Closes every socket owned by the listener. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_listenerLock);
    if (!g_listener.active) {
        ReleaseSRWLockExclusive(&g_listenerLock);
        return;
    }
    g_listener.active = false;
    if (g_listener.acceptor != INVALID_SOCKET) {
        closesocket(g_listener.acceptor);
        g_listener.acceptor = INVALID_SOCKET;
    }
    for (std::size_t slot = 0; slot < g_listener.peers.size(); ++slot) {
        close_peer(slot);
    }
    g_listener.nextPollTick = 0;
    if (g_listener.winsockOwned) {
        WSACleanup();
        g_listener.winsockOwned = false;
    }
    ReleaseSRWLockExclusive(&g_listenerLock);
}

} // namespace sunrise::server::transport
