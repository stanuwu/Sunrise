#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::nat_punch {

inline constexpr std::uint32_t kMessageType = 0x21;

inline constexpr std::size_t kRecordSize = 0xA8;

inline constexpr std::size_t kAddressSize = 0x56;

inline constexpr std::size_t kPayloadCapacity = kRecordSize - 0x88;

namespace offset {
/**
 * `+0x00` u8, set to 1. Zero logs "Invalid target transport address, can not verify we're the
 * intended receipient."
 */
inline constexpr std::size_t kHasTarget = 0x00;
inline constexpr std::size_t kTargetEngaged = 0x08;
/** `+0x10` u64, set to `0x56`. A different value logs "invalid transport secure address size". */
inline constexpr std::size_t kTargetSize = 0x10;
inline constexpr std::size_t kTargetAddress = 0x18;
inline constexpr std::size_t kSourceEngaged = 0x70;
inline constexpr std::size_t kSourceTag = 0x74;
inline constexpr std::size_t kHasPayload = 0x78;
inline constexpr std::size_t kPayloadSize = 0x80;
inline constexpr std::size_t kPayload = 0x88;
} // namespace offset

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
inline constexpr std::size_t kKind = 0x00;
inline constexpr std::size_t kIdentifier = 0x01;
inline constexpr std::size_t kTag = 0x09;
inline constexpr std::size_t kLength = 0x0D;
inline constexpr std::size_t kBlob = 0x11;
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
