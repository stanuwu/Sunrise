#include "reliable_assembly.h"

#include <algorithm>

#include "../../encoding/bit_reader.h"
#include "peer_container.h"

namespace sunrise::middleware::gameplay::peer {
namespace {
namespace gp = state::gameplay;
constexpr std::uint8_t kIdWidth = 6;
constexpr std::uint8_t kSizeWidth = 18;
constexpr std::size_t kByteBits = 8;
static_assert(gp::kMessageSequenceModulus % gp::kReliableSlots == 0);
static_assert(gp::kReliableSlots >= kMaximumRecords);

[[nodiscard]] std::uint16_t distance(std::uint16_t from, std::uint16_t to) noexcept {
    return static_cast<std::uint16_t>((to + gp::kMessageSequenceModulus - from)
                                      % gp::kMessageSequenceModulus);
}
[[nodiscard]] std::size_t slot_of(std::uint16_t sequence) noexcept {
    return sequence % gp::kReliableSlots;
}

} // namespace

std::size_t accept_records(const QueueRecords& records, gp::ReliableQueue& queue) noexcept {
    if (records.count > records.records.size()) {
        return records.count;
    }
    std::size_t dropped = 0;
    for (std::size_t index = 0; index < records.count; ++index) {
        const auto& record = records.records[index];
        if (record.sequence >= gp::kMessageSequenceModulus) {
            ++dropped;
            continue;
        }
        if (!queue.started) {
            queue.started = true;
            queue.nextSequence = record.sequence;
        }
        if (distance(queue.nextSequence, record.sequence) >= gp::kReliableSlots) {
            ++dropped;
            continue;
        }
        auto& fragment = queue.fragments[slot_of(record.sequence)];
        if (fragment.occupied && fragment.sequence == record.sequence) {
            continue;
        }
        fragment.sequence = record.sequence;
        fragment.bitCount = record.bitCount;
        fragment.shortFragment = record.shortFragment;
        fragment.bytes = record.bytes;
        fragment.occupied = true;
    }
    return dropped;
}

bool drain_message(gp::ReliableQueue& queue, AssembledMessage& output) noexcept {
    if (!queue.started) {
        return false;
    }
    for (;;) {
        auto& fragment = queue.fragments[slot_of(queue.nextSequence)];
        if (!fragment.occupied || fragment.sequence != queue.nextSequence) {
            return false;
        }
        const bool final = fragment.shortFragment;
        // Full fragments on both native queues are byte aligned. Only a terminator may end in
        // a partial byte. Keep draining a refused run so its next valid message can proceed.
        queue.discarding =
            queue.discarding || fragment.bitCount > fragment.bytes.size() * kByteBits
            || (!final && fragment.bitCount % kByteBits != 0)
            || fragment.bitCount > gp::kReassemblyCapacity * kByteBits - queue.assemblyBits;
        if (!queue.discarding) {
            const auto bytes = (fragment.bitCount + kByteBits - 1) / kByteBits;
            std::copy_n(fragment.bytes.begin(),
                        bytes,
                        queue.assembly.begin()
                            + static_cast<std::ptrdiff_t>(queue.assemblyBits / kByteBits));
            queue.assemblyBits += fragment.bitCount;
            if (queue.assemblyBits % kByteBits != 0) {
                queue.assembly[queue.assemblyBits / kByteBits] &=
                    std::byte{static_cast<unsigned char>(
                        0xFFU << (kByteBits - queue.assemblyBits % kByteBits))};
            }
        }
        fragment = {};
        queue.nextSequence =
            static_cast<std::uint16_t>((queue.nextSequence + 1) % gp::kMessageSequenceModulus);
        if (!final) {
            continue;
        }

        const bool refused = queue.discarding;
        const auto bitCount = queue.assemblyBits;
        queue.assemblyBits = 0;
        queue.discarding = false;
        if (refused || bitCount < kIdWidth + kSizeWidth) {
            continue;
        }
        encoding::bits::Reader header(std::span(queue.assembly).first((bitCount + 7) / 8));
        std::uint64_t id{}, declared{};
        if (!header.read(kIdWidth, id) || id > kMaximumMessageId
            || !header.read(kSizeWidth, declared)) {
            continue;
        }
        AssembledMessage candidate{};
        std::copy_n(queue.assembly.begin(), (bitCount + 7) / 8, candidate.bytes.begin());
        candidate.bitCount = bitCount;
        candidate.id = static_cast<std::uint8_t>(id);
        candidate.declaredSize = static_cast<std::uint32_t>(declared);
        candidate.bodyBitOffset = kIdWidth + kSizeWidth;
        output = candidate;
        return true;
    }
}
} // namespace sunrise::middleware::gameplay::peer
