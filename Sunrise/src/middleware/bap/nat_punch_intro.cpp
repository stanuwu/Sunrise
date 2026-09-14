#include "nat_punch_intro.h"

#include <cstring>

#include "../encoding/byte_order.h"

namespace sunrise::middleware::bap::nat_punch {
namespace {

template <typename Value>
[[nodiscard]] Value read(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(Value));
    return value;
}

template <typename Value>
void write(std::array<std::byte, kRecordSize>& bytes, std::size_t offset, Value value) noexcept {
    std::memcpy(bytes.data() + offset, &value, sizeof(Value));
}

} // namespace

bool parse(std::span<const std::byte> bytes, IntroRecord& record) noexcept {
    record = IntroRecord{};
    if (bytes.size() < kRecordSize) {
        return false;
    }
    const std::uint64_t declared = read<std::uint64_t>(bytes, offset::kPayloadSize);
    if (declared > kPayloadCapacity) {
        return false;
    }
    // The size word at `+0x10` is the receiver's own guard: a value other than 0x56 makes it log
    // "invalid transport secure address size" rather than treat the blob as an address. Refuse it
    // here for the same reason -- an 86-byte compare against a differently sized blob is not a
    // compare at all.
    if (read<std::uint64_t>(bytes, offset::kTargetSize) != kAddressSize) {
        return false;
    }
    record.hasTarget = read<std::uint8_t>(bytes, offset::kHasTarget) != 0;
    record.hasPayload = read<std::uint8_t>(bytes, offset::kHasPayload) != 0;
    record.sourceTag = read<std::uint32_t>(bytes, offset::kSourceTag);
    record.payloadSize = declared;
    std::memcpy(record.targetAddress.data(), bytes.data() + offset::kTargetAddress, kAddressSize);
    std::memcpy(record.payload.data(), bytes.data() + offset::kPayload, kPayloadCapacity);
    return true;
}

bool compose(const IntroRecord& record, std::array<std::byte, kRecordSize>& bytes) noexcept {
    bytes = {};
    if (record.payloadSize > kPayloadCapacity) {
        return false;
    }
    write<std::uint8_t>(bytes, offset::kHasTarget, record.hasTarget ? 1U : 0U);
    write<std::uint8_t>(bytes, offset::kTargetEngaged, 1U);
    write<std::uint64_t>(bytes, offset::kTargetSize, kAddressSize);
    std::memcpy(bytes.data() + offset::kTargetAddress, record.targetAddress.data(), kAddressSize);
    write<std::uint8_t>(bytes, offset::kSourceEngaged, 1U);
    write<std::uint32_t>(bytes, offset::kSourceTag, record.sourceTag);
    write<std::uint8_t>(bytes, offset::kHasPayload, record.hasPayload ? 1U : 0U);
    write<std::uint64_t>(bytes, offset::kPayloadSize, record.payloadSize);
    std::memcpy(bytes.data() + offset::kPayload, record.payload.data(), kPayloadCapacity);
    return true;
}

bool parse_envelope(std::span<const std::byte> bytes,
                    EnvelopeHeader& header,
                    std::span<const std::byte>& blob) noexcept {
    header = EnvelopeHeader{};
    blob = {};
    if (bytes.size() < envelope::kHeaderSize) {
        return false;
    }
    if (read<std::uint8_t>(bytes, envelope::kKind) != envelope::kKindValue) {
        return false;
    }
    const std::uint32_t length =
        encoding::read_u32_be(bytes.subspan(envelope::kLength).first<encoding::kU32Size>());
    if (length > envelope::kBlobCapacity || bytes.size() - envelope::kHeaderSize < length) {
        return false;
    }
    header.identifier =
        encoding::read_u64_be(bytes.subspan(envelope::kIdentifier).first<encoding::kU64Size>());
    header.tag = encoding::read_u32_be(bytes.subspan(envelope::kTag).first<encoding::kU32Size>());
    blob = bytes.subspan(envelope::kHeaderSize, length);
    return true;
}

bool compose_envelope(const EnvelopeHeader& header,
                      std::span<const std::byte> blob,
                      std::span<std::byte> output,
                      std::size_t& written) noexcept {
    written = 0;
    if (blob.size() > envelope::kBlobCapacity
        || output.size() < envelope::kHeaderSize + blob.size()) {
        return false;
    }
    output[envelope::kKind] = static_cast<std::byte>(envelope::kKindValue);
    encoding::write_u64_be(output.subspan(envelope::kIdentifier).first<encoding::kU64Size>(),
                           header.identifier);
    encoding::write_u32_be(output.subspan(envelope::kTag).first<encoding::kU32Size>(), header.tag);
    encoding::write_u32_be(output.subspan(envelope::kLength).first<encoding::kU32Size>(),
                           static_cast<std::uint32_t>(blob.size()));
    for (std::size_t index = 0; index < blob.size(); ++index) {
        output[envelope::kHeaderSize + index] = blob[index];
    }
    written = envelope::kHeaderSize + blob.size();
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
