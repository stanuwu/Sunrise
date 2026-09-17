#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::nat_punch {

/** Native intro emitter (RVA 0xF21600) passes opcode 0x21 to the BAP send. */
inline constexpr std::uint32_t kMessageType = 0x21;

/** The record length that same send declares, and the length the native reader reports back. */
inline constexpr std::size_t kRecordSize = 0xA8;

/** One secure address: the extent the native copy loop measures and the reader compares over. */
inline constexpr std::size_t kAddressSize = 0x56;

namespace offset {
/**
 * `+0x00` u8, set to 1. Zero logs "Invalid target transport address, can not verify we're the
 * intended receipient."
 */
inline constexpr std::size_t kHasTarget = 0x00;
/** `+0x08` u8 engaged flag. The native writer sets it to 1 and the native reader never reads it. */
inline constexpr std::size_t kTargetEngaged = 0x08;
/** `+0x10` u64, set to `0x56`. A different value logs "invalid transport secure address size". */
inline constexpr std::size_t kTargetSize = 0x10;
/** `+0x18`, the intended recipient's secure address, `kAddressSize` bytes of it. */
inline constexpr std::size_t kTargetAddress = 0x18;
/** `+0x70` u8, the source half's engaged flag. Written 1, and again never read back. */
inline constexpr std::size_t kSourceEngaged = 0x70;
/** `+0x74` u32, the tag the native emitter mints for one intro. */
inline constexpr std::size_t kSourceTag = 0x74;
/** `+0x78` u8. Zero logs "with no payload, can not forward to transport". */
inline constexpr std::size_t kHasPayload = 0x78;
/** `+0x80` u64, the payload length the native drain passes on. */
inline constexpr std::size_t kPayloadSize = 0x80;
/** `+0x88`, the opaque punch payload, running to the end of the record. */
inline constexpr std::size_t kPayload = 0x88;
} // namespace offset

/** The record's tail past its last fixed field, so all one punch payload may occupy. */
inline constexpr std::size_t kPayloadCapacity = kRecordSize - offset::kPayload;

struct IntroRecord final {
    std::array<std::byte, kAddressSize> targetAddress{};
    std::array<std::byte, kPayloadCapacity> payload{};
    std::uint64_t payloadSize{};
    std::uint32_t sourceTag{};
    bool hasTarget{};
    bool hasPayload{};
};

[[nodiscard]] bool parse(std::span<const std::byte> bytes, IntroRecord& record) noexcept;

[[nodiscard]] bool compose(const IntroRecord& record,
                           std::array<std::byte, kRecordSize>& bytes) noexcept;

namespace envelope {
/** Header fields in order: u8 kind, big-endian u64 identifier, u32 tag, u32 length, then blob. */
inline constexpr std::size_t kKind = 0x00;
inline constexpr std::size_t kIdentifier = kKind + 1;
inline constexpr std::size_t kTag = kIdentifier + 8;
inline constexpr std::size_t kLength = kTag + 4;
inline constexpr std::size_t kBlob = kLength + 4;
inline constexpr std::size_t kHeaderSize = kBlob;
/** Size of the scratch buffer the native emitter allocates for one intro blob. */
inline constexpr std::size_t kBlobCapacity = 0x7D800;
/** The only kind byte either side accepts. */
inline constexpr std::uint8_t kKindValue = 1;
inline constexpr bool kIdentifierIsBigEndian = true;
} // namespace envelope

struct EnvelopeHeader final {
    std::uint64_t identifier{};
    std::uint32_t tag{};
};

[[nodiscard]] bool parse_envelope(std::span<const std::byte> bytes,
                                  EnvelopeHeader& header,
                                  std::span<const std::byte>& blob) noexcept;

[[nodiscard]] bool compose_envelope(const EnvelopeHeader& header,
                                    std::span<const std::byte> blob,
                                    std::span<std::byte> output,
                                    std::size_t& written) noexcept;

[[nodiscard]] bool retarget_envelope(std::span<const std::byte> body,
                                     const std::array<std::byte, kAddressSize>& recipientAddress,
                                     std::span<std::byte> output,
                                     std::size_t& written) noexcept;

void retarget(IntroRecord& record,
              const std::array<std::byte, kAddressSize>& recipientAddress) noexcept;

} // namespace sunrise::middleware::bap::nat_punch
