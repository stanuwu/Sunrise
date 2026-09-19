#include "outbound_window.h"

#include <algorithm>

namespace sunrise::middleware::gameplay::peer::outbound_window {
std::size_t count(const state::gameplay::OutboundQueue& queue) noexcept {
    return queue.awaitingAcknowledgement
               ? std::min(queue.carriedCount, queue.count)
               : std::min(state::gameplay::kOutboundSendWindow, queue.count);
}

void begin(state::gameplay::OutboundQueue& queue, std::uint16_t packet) noexcept {
    if (!queue.awaitingAcknowledgement && queue.count) {
        queue.carriedCount = count(queue);
        queue.sentInPacket = packet;
        queue.awaitingAcknowledgement = true;
    }
}

bool acknowledge(state::gameplay::OutboundQueue& queue, const AckState& ack) noexcept {
    if (!queue.awaitingAcknowledgement || !queue.carriedCount || queue.carriedCount > queue.count
        || !acknowledgement_covers(ack, queue.sentInPacket)) {
        return false;
    }
    const auto remaining = queue.count - queue.carriedCount;
    for (std::size_t index = 0; index < remaining; ++index) {
        queue.fragments[index] = queue.fragments[index + queue.carriedCount];
    }
    for (std::size_t index = remaining; index < queue.count; ++index) {
        queue.fragments[index] = {};
    }
    queue.count = remaining;
    queue.carriedCount = 0;
    queue.awaitingAcknowledgement = false;
    return true;
}

void send_failed(state::gameplay::OutboundQueue& queue, std::uint16_t packet) noexcept {
    if (queue.awaitingAcknowledgement && queue.sentInPacket == packet) {
        queue.carriedCount = 0;
        queue.awaitingAcknowledgement = false;
    }
}
} // namespace sunrise::middleware::gameplay::peer::outbound_window
