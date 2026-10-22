#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../state/gameplay/definition.h"
#include "established_packet.h"

namespace sunrise::middleware::gameplay::peer {

/** One message reassembled from a contiguous run of fragments. */
struct AssembledMessage {
    std::array<std::byte, state::gameplay::kReassemblyCapacity> bytes{};
    /** Message bits, before the inner header is read. */
    std::size_t bitCount{};
    std::uint8_t id{};
    /** Decoded C-structure size the registry declares for that id. */
    std::uint32_t declaredSize{};
    /** Body bit offset, just after the inner header. */
    std::size_t bodyBitOffset{};
};

/**
 * Files one packet's records into a queue.
 * A record outside the buffered window is refused. The window covers one maximum-sized packet;
 * contiguous fragments retire into the assembly buffer before the next packet arrives.
 * @param records Records the packet carried, in wire order.
 * @param queue Queue receiving them.
 * @return How many records the window refused.
 */
std::size_t accept_records(const QueueRecords& records,
                           state::gameplay::ReliableQueue& queue) noexcept;

/**
 * Takes the next complete message off a queue.
 * A message is a contiguous run of fragments ending in a short one. An incomplete run stays
 * buffered until its missing fragment arrives. Contiguous prefixes leave the fragment window
 * while awaiting the terminator. Oversized or malformed runs are discarded through that terminator.
 * @param queue Queue to drain.
 * @param output Receives the message and its decoded inner header.
 * @return True when one whole message was drained.
 */
[[nodiscard]] bool drain_message(state::gameplay::ReliableQueue& queue,
                                 AssembledMessage& output) noexcept;

} // namespace sunrise::middleware::gameplay::peer
