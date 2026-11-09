#include <WinSock2.h>
#include <array>
#include <cstdio>
#include <cstring>

#include "../../core/logging/log.h"
#include "../bap/runtime.h"
#include "internal.h"

namespace sunrise::server::transport {
namespace {

/** @param prefix At least one buffered byte. @return Whole frame size, or zero when unknown. */
[[nodiscard]] std::size_t frame_size(std::span<const std::byte> prefix) noexcept {
    if (prefix.size() < kOuterHeaderSize) {
        return 0;
    }
    std::uint32_t payload = 0;
    for (std::size_t index = 0; index < sizeof payload; ++index) {
        payload =
            (payload << 8U) | std::to_integer<std::uint32_t>(prefix[kOuterLengthOffset + index]);
    }
    const std::size_t total = kOuterHeaderSize + payload;
    return total <= kStreamCapacity ? total : 0;
}

} // namespace

/** Offers one event to the Server and stages whatever it produces. */
bool offer(Peer& peer, client::network::BapEvent event, std::span<const std::byte> frame) noexcept {
    if (peer.outputSize != 0) {
        return false;
    }
    client::network::BapResponse response{};
    const client::network::BapRequest request{
        event, peer.connectionId, frame, peer.output, peer.remoteAddress};
    const bool handled = bap::consume(request, response);
    if (response.closeConnection) {
        return false;
    }
    peer.authenticated = response.authenticated;
    peer.inputDeferred = response.deferFrame;
    if (!handled || response.size == 0) {
        return true;
    }
    if (response.size > peer.output.size()) {
        return false;
    }
    peer.outputOffset = 0;
    peer.outputSize = response.size;
    peer.outputProgressTick = peer.serviceTick;
    return true;
}

/** Removes and offers at most one complete frame from one peer's stream. */
bool drain_stream(Peer& peer) noexcept {
    if (peer.outputSize != 0) {
        return true;
    }
    const auto pending = std::span(peer.stream).first(peer.streamSize);
    const std::size_t total = frame_size(pending);
    if (total == 0) {
        return peer.streamSize != kStreamCapacity;
    }
    if (pending.size() < total) {
        return true;
    }
    // Every inbound frame is named here, so a frame the Server drops is still accounted for.
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=transport stage=frame conn=%u type=%u bytes=%zu",
                                    peer.connectionId,
                                    static_cast<unsigned>(pending[1]),
                                    total);
    if (count > 0) {
        const std::size_t length = static_cast<std::size_t>(count) < line.size()
                                       ? static_cast<std::size_t>(count)
                                       : line.size() - 1;
        core::log::write(
            core::log::Channel::server, core::log::Level::debug, {line.data(), length});
    }
    if (!offer(peer, client::network::BapEvent::frame, pending.first(total))) {
        return false;
    }
    if (peer.inputDeferred) {
        return true;
    }
    peer.streamSize -= total;
    peer.inputStartedTick = peer.serviceTick;
    if (peer.streamSize != 0) {
        std::memmove(peer.stream.data(), peer.stream.data() + total, peer.streamSize);
    }
    return true;
}

/** Advances one committed output by an accepted send count. */
bool advance_output(Peer& peer, std::size_t sent) noexcept {
    if (sent == 0 || peer.outputSize == 0 || peer.outputOffset >= peer.outputSize
        || sent > peer.outputSize - peer.outputOffset) {
        return false;
    }
    peer.outputOffset += sent;
    peer.outputProgressTick = peer.serviceTick;
    if (peer.outputOffset == peer.outputSize) {
        peer.outputOffset = 0;
        peer.outputSize = 0;
    }
    return true;
}

/** Closes one peer and reports its session end to the Server. */
void close_peer(Peer& peer) noexcept {
    if (peer.socket == INVALID_SOCKET) {
        return;
    }
    client::network::BapResponse response{};
    const client::network::BapRequest request{
        client::network::BapEvent::close, peer.connectionId, {}, {}};
    (void)bap::consume(request, response);
    closesocket(peer.socket);
    peer.socket = INVALID_SOCKET;
    peer.connectionId = 0;
    peer.inputDeferred = false;
    peer.authenticated = false;
    peer.streamSize = 0;
    peer.outputOffset = 0;
    peer.outputSize = 0;
}

} // namespace sunrise::server::transport
