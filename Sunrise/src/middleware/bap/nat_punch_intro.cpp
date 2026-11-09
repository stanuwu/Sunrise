#include "nat_punch_intro.h"

#include <cstring>

#include "../encoding/byte_order.h"

namespace sunrise::middleware::bap::nat_punch {

bool parse(std::span<const std::byte> bytes, IntroRecord& record) noexcept {
    record = IntroRecord{};
    if (bytes.size() < kRecordSize) {
        return false;
    }
    IntroWire wire{};
    std::memcpy(&wire, bytes.data(), sizeof wire);
    // A differently sized address cannot be compared over `kAddressSize` bytes.
    if (wire.payloadSize > kPayloadCapacity || wire.targetSize != kAddressSize) {
        return false;
    }
    record.hasTarget = wire.hasTarget != 0;
    record.hasPayload = wire.hasPayload != 0;
    record.sourceTag = wire.sourceTag;
    record.payloadSize = wire.payloadSize;
    record.targetAddress = wire.targetAddress;
    record.payload = wire.payload;
    return true;
}

bool compose(const IntroRecord& record, std::array<std::byte, kRecordSize>& bytes) noexcept {
    bytes = {};
    if (record.payloadSize > kPayloadCapacity) {
        return false;
    }
    IntroWire wire{};
    wire.hasTarget = record.hasTarget ? 1U : 0U;
    wire.targetEngaged = 1U;
    wire.targetSize = kAddressSize;
    wire.targetAddress = record.targetAddress;
    wire.sourceEngaged = 1U;
    wire.sourceTag = record.sourceTag;
    wire.hasPayload = record.hasPayload ? 1U : 0U;
    wire.payloadSize = record.payloadSize;
    wire.payload = record.payload;
    std::memcpy(bytes.data(), &wire, sizeof wire);
    return true;
}

bool parse_envelope(std::span<const std::byte> bytes,
                    EnvelopeHeader& header,
                    std::span<const std::byte>& blob) noexcept {
    header = EnvelopeHeader{};
    blob = {};
    if (bytes.size() < sizeof(EnvelopeWire)) {
        return false;
    }
    EnvelopeWire wire{};
    std::memcpy(&wire, bytes.data(), sizeof wire);
    const std::uint32_t length = encoding::read_u32_be(wire.length);
    if (wire.kind != kEnvelopeKind || length > kBlobCapacity
        || bytes.size() - sizeof(EnvelopeWire) < length) {
        return false;
    }
    header.identifier = encoding::read_u64_be(wire.identifier);
    header.tag = encoding::read_u32_be(wire.tag);
    blob = bytes.subspan(sizeof(EnvelopeWire), length);
    return true;
}

bool compose_envelope(const EnvelopeHeader& header,
                      std::span<const std::byte> blob,
                      std::span<std::byte> output,
                      std::size_t& written) noexcept {
    written = 0;
    if (blob.size() > kBlobCapacity || output.size() < sizeof(EnvelopeWire) + blob.size()) {
        return false;
    }
    EnvelopeWire wire{};
    wire.kind = kEnvelopeKind;
    encoding::write_u64_be(wire.identifier, header.identifier);
    encoding::write_u32_be(wire.tag, header.tag);
    encoding::write_u32_be(wire.length, static_cast<std::uint32_t>(blob.size()));
    std::memcpy(output.data(), &wire, sizeof wire);
    std::memcpy(output.data() + sizeof wire, blob.data(), blob.size());
    written = sizeof wire + blob.size();
    return true;
}

bool retarget_envelope(std::span<const std::byte> body,
                       const std::array<std::byte, kAddressSize>& recipientAddress,
                       std::span<std::byte> output,
                       std::size_t& written) noexcept {
    written = 0;
    EnvelopeHeader header{};
    std::span<const std::byte> blob{};
    if (!parse_envelope(body, header, blob)) {
        return false;
    }
    IntroRecord record{};
    if (!parse(blob, record)) {
        return false;
    }
    retarget(record, recipientAddress);
    std::array<std::byte, kRecordSize> rewritten{};
    if (!compose(record, rewritten)) {
        return false;
    }
    return compose_envelope(header, rewritten, output, written);
}

void retarget(IntroRecord& record,
              const std::array<std::byte, kAddressSize>& recipientAddress) noexcept {
    record.targetAddress = recipientAddress;
    record.hasTarget = true;
}

} // namespace sunrise::middleware::bap::nat_punch