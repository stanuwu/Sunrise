#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::nat_punch {

/** Native intro emitter (RVA 0xF21600) passes opcode 0x21 to the BAP send. */
inline constexpr std::uint32_t kMessageType = 0x21;

/** One secure address: the extent the native copy loop measures and the reader compares over. */
inline constexpr std::size_t kAddressSize = 0x56;

/** The record's tail past its last fixed field, so all one punch payload may occupy. */
inline constexpr std::size_t kPayloadCapacity = 0x20;

#pragma pack(push, 1)
/** The native intro record, little-endian, as the native send declares and the reader reports. */
struct IntroWire final {
    /** Set to 1. Zero logs "Invalid target transport address". */
    std::uint8_t hasTarget;
    std::array<std::byte, 7> gap1;
    /** The native writer sets it to 1 and the native reader never reads it. */
    std::uint8_t targetEngaged;
    std::array<std::byte, 7> gap9;
    /** Set to `kAddressSize`. Another value logs "invalid transport secure address size". */
    std::uint64_t targetSize;
    /** The intended recipient's secure address. */
    std::array<std::byte, kAddressSize> targetAddress;
    std::array<std::byte, 2> gap6e;
    /** The source half's engaged flag. Written 1 and never read back. */
    std::uint8_t sourceEngaged;
    std::array<std::byte, 3> gap71;
    /** The tag the native emitter mints for one intro. */
    std::uint32_t sourceTag;
    /** Zero logs "with no payload, can not forward to transport". */
    std::uint8_t hasPayload;
    std::array<std::byte, 7> gap79;
    /** The payload length the native drain passes on. */
    std::uint64_t payloadSize;
    /** The opaque punch payload, running to the end of the record. */
    std::array<std::byte, kPayloadCapacity> payload;
};

/** Envelope header; every multi-byte field is big-endian, unlike the record. */
struct EnvelopeWire final {
    /** The only accepted value is `kEnvelopeKind`. */
    std::uint8_t kind;
    std::array<std::byte, 8> identifier;
    std::array<std::byte, 4> tag;
    std::array<std::byte, 4> length;
};
#pragma pack(pop)

/** The record length the native send declares. */
inline constexpr std::size_t kRecordSize = sizeof(IntroWire);
static_assert(kRecordSize == 0xA8);
static_assert(offsetof(IntroWire, targetAddress) == 0x18);
static_assert(offsetof(IntroWire, sourceTag) == 0x74);
static_assert(offsetof(IntroWire, payload) == 0x88);

/** The only kind byte either side accepts. */
inline constexpr std::uint8_t kEnvelopeKind = 1;
/** Size of the scratch buffer the native emitter allocates for one intro blob. */
inline constexpr std::size_t kBlobCapacity = 0x7D800;

/** One parsed intro record: the target it names, its opaque punch payload, and the source tag. */
struct IntroRecord final {
    std::array<std::byte, kAddressSize> targetAddress{};
    std::array<std::byte, kPayloadCapacity> payload{};
    std::uint64_t payloadSize{};
    std::uint32_t sourceTag{};
    bool hasTarget{};
    bool hasPayload{};
};

/**
 * Parses one fixed-size intro record.
 * @return False for a short buffer, a payload declared larger than `kPayloadCapacity`, or an
 * address-size guard word other than `kAddressSize`.
 */
[[nodiscard]] bool parse(std::span<const std::byte> bytes, IntroRecord& record) noexcept;

/** @return False when `record.payloadSize` exceeds `kPayloadCapacity`. */
[[nodiscard]] bool compose(const IntroRecord& record,
                           std::array<std::byte, kRecordSize>& bytes) noexcept;

/** One envelope's routing identifier and the source's per-intro tag. */
struct EnvelopeHeader final {
    std::uint64_t identifier{};
    std::uint32_t tag{};
};

/**
 * Parses one envelope header and the blob it wraps.
 * @return False for a short buffer, a kind byte other than `kEnvelopeKind`, or a declared length
 * exceeding `kBlobCapacity` or the bytes remaining after the header.
 */
[[nodiscard]] bool parse_envelope(std::span<const std::byte> bytes,
                                  EnvelopeHeader& header,
                                  std::span<const std::byte>& blob) noexcept;

/** @return False when `blob` exceeds `kBlobCapacity` or `output` is too small for the result. */
[[nodiscard]] bool compose_envelope(const EnvelopeHeader& header,
                                    std::span<const std::byte> blob,
                                    std::span<std::byte> output,
                                    std::size_t& written) noexcept;

/**
 * Parses `body`'s envelope and intro record, retargets it to `recipientAddress`, and recomposes
 * both under the original identifier and tag.
 * @return False when `body` fails to parse or `output` is too small for the result.
 */
[[nodiscard]] bool retarget_envelope(std::span<const std::byte> body,
                                     const std::array<std::byte, kAddressSize>& recipientAddress,
                                     std::span<std::byte> output,
                                     std::size_t& written) noexcept;

/** Points `record` at `recipientAddress`, replacing its target address and marking it targeted. */
void retarget(IntroRecord& record,
              const std::array<std::byte, kAddressSize>& recipientAddress) noexcept;

} // namespace sunrise::middleware::bap::nat_punch