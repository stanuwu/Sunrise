#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "entity_slots.h"

namespace sunrise::middleware::bap::activity_message::host_control {

/** Exact Activity Message IDs for this host-to-client codec group. */
inline constexpr std::uint32_t kClaimAuthorityMessageType = 24;
inline constexpr std::uint32_t kPurgeAuthorityMessageType = 25;
inline constexpr std::uint32_t kResetAuthorityMaskMessageType = 28;
inline constexpr std::uint32_t kQueryAuthorityMaskMessageType = 30;

/** Fixed and bounded wire sizes, one per root definition. */
inline constexpr std::size_t kClaimAuthorityBitCount = 8 + entity_slots::kSlotCount + 3;
inline constexpr std::size_t kClaimAuthorityByteCount = (kClaimAuthorityBitCount + 7) / 8;
inline constexpr std::size_t kPurgeAuthorityBitCount = 3 + 8 + entity_slots::kSlotCount;
inline constexpr std::size_t kPurgeAuthorityByteCount = (kPurgeAuthorityBitCount + 7) / 8;
inline constexpr std::size_t kCorrelationBitCount = 32;
inline constexpr std::size_t kCorrelationByteCount = kCorrelationBitCount / 8;
inline constexpr std::size_t kReservationCapacity = 32;
inline constexpr std::size_t kReservationKeySize = 8;
inline constexpr std::size_t kReservationCountBitCount = 6;
inline constexpr std::size_t kReservationsFailedMaximumBitCount =
    kReservationCountBitCount + kReservationCapacity * kReservationKeySize * 8;
/** Message-45 grows with its key count; this bounds the body at full capacity. */
inline constexpr std::size_t kReservationsFailedMaximumByteCount =
    (kReservationsFailedMaximumBitCount + 7) / 8;
/** Message-56 carries a 32-bit mask; message-57 carries one bit padded to a byte. */
inline constexpr std::size_t kPerfRequestKillBitCount = 32;
inline constexpr std::size_t kPerfRequestKillByteCount = kPerfRequestKillBitCount / 8;
inline constexpr std::size_t kPerfRequestReflectBitCount = 1;
inline constexpr std::size_t kPerfRequestReflectByteCount = 1;

/** Three-bit bias-one values decode to -1 through 6. */
inline constexpr std::int8_t kMinimumReason = -1;
inline constexpr std::int8_t kMaximumReason = 6;

/** Complete message-24 body. The mask uses canonical slot bytes. */
struct ClaimAuthorityBody final {
    entity_slots::EntitySlotMask slots{};
    std::uint8_t epoch{};
    std::int8_t reason{};
};

/** Complete message-25 body. The mask uses canonical slot bytes. */
struct PurgeAuthorityBody final {
    entity_slots::EntitySlotMask slots{};
    std::uint8_t epoch{};
    std::int8_t reason{};
};

/** Complete message-28 or message-30 biased correlation body. */
struct AuthorityMaskRequestBody final {
    std::int32_t correlation{};
};

/** One reservation key as eight reflected bytes in struct order. */
using ReservationKey = std::array<std::byte, kReservationKeySize>;

/** Complete bounded message-45 body. */
struct ReservationsFailedBody final {
    std::array<ReservationKey, kReservationCapacity> keys{};
    std::size_t keyCount{};
};

/** Complete message-56 body. Its consumer is inert in this client build. */
struct PerfRequestKillBody final {
    std::int32_t value{};
};

/** Complete message-57 body. Its consumer is inert in this client build. */
struct PerfRequestReflectBody final {
    bool value{};
};

/** Encodes one complete message-24 body without changing a refused output buffer. */
[[nodiscard]] bool encode_claim_authority(const ClaimAuthorityBody& body,
                                          std::span<std::byte> output,
                                          std::size_t& written) noexcept;
/** Decodes one exact message-24 body without changing the destination on failure. */
[[nodiscard]] bool decode_claim_authority(std::span<const std::byte> input,
                                          ClaimAuthorityBody& body) noexcept;
/** @return True when the input is one complete message-24 body. */
[[nodiscard]] bool validate_retirement_authority(std::span<const std::byte> input) noexcept;

/** Encodes one complete message-25 body without changing a refused output buffer. */
[[nodiscard]] bool encode_purge_authority(const PurgeAuthorityBody& body,
                                          std::span<std::byte> output,
                                          std::size_t& written) noexcept;
/** Decodes one exact message-25 body without changing the destination on failure. */
[[nodiscard]] bool decode_purge_authority(std::span<const std::byte> input,
                                          PurgeAuthorityBody& body) noexcept;
/** @return True when the input is one complete message-25 body. */
[[nodiscard]] bool validate_purge_authority(std::span<const std::byte> input) noexcept;

/** Encodes one complete message-28 body without changing a refused output buffer. */
[[nodiscard]] bool encode_reset_authority_mask(const AuthorityMaskRequestBody& body,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;
/** Decodes one exact message-28 body without changing the destination on failure. */
[[nodiscard]] bool decode_reset_authority_mask(std::span<const std::byte> input,
                                               AuthorityMaskRequestBody& body) noexcept;
/** @return True when the input is one complete message-28 body. */
[[nodiscard]] bool validate_reset_authority_mask(std::span<const std::byte> input) noexcept;

/** Encodes one complete message-30 body without changing a refused output buffer. */
[[nodiscard]] bool encode_query_authority_mask(const AuthorityMaskRequestBody& body,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;
/** Decodes one exact message-30 body without changing the destination on failure. */
[[nodiscard]] bool decode_query_authority_mask(std::span<const std::byte> input,
                                               AuthorityMaskRequestBody& body) noexcept;
/** @return True when the input is one complete message-30 body. */
[[nodiscard]] bool validate_query_authority_mask(std::span<const std::byte> input) noexcept;

/** @return Exact message-45 bit count, or zero when the key count is unsafe. */
[[nodiscard]] std::size_t reservations_failed_bit_count(std::size_t keyCount) noexcept;
/** @return Exact message-45 byte count, or zero when the key count is unsafe. */
[[nodiscard]] std::size_t reservations_failed_byte_count(std::size_t keyCount) noexcept;
/** Encodes one complete message-45 body without changing a refused output buffer. */
[[nodiscard]] bool encode_reservations_failed(const ReservationsFailedBody& body,
                                              std::span<std::byte> output,
                                              std::size_t& written) noexcept;
/** Decodes one exact bounded message-45 body without changing the destination on failure. */
[[nodiscard]] bool decode_reservations_failed(std::span<const std::byte> input,
                                              ReservationsFailedBody& body) noexcept;
/** @return True when the input is one complete bounded message-45 body. */
[[nodiscard]] bool validate_reservations_failed(std::span<const std::byte> input) noexcept;

/** Encodes one complete message-56 body without changing a refused output buffer. */
[[nodiscard]] bool encode_perf_request_kill(const PerfRequestKillBody& body,
                                            std::span<std::byte> output,
                                            std::size_t& written) noexcept;
/** Decodes one exact message-56 body without changing the destination on failure. */
[[nodiscard]] bool decode_perf_request_kill(std::span<const std::byte> input,
                                            PerfRequestKillBody& body) noexcept;
/** @return True when the input is one complete message-56 body. */
[[nodiscard]] bool validate_perf_request_kill(std::span<const std::byte> input) noexcept;

/** Encodes one complete message-57 body without changing a refused output buffer. */
[[nodiscard]] bool encode_perf_request_reflect(const PerfRequestReflectBody& body,
                                               std::span<std::byte> output,
                                               std::size_t& written) noexcept;
/** Decodes one exact message-57 body without changing the destination on failure. */
[[nodiscard]] bool decode_perf_request_reflect(std::span<const std::byte> input,
                                               PerfRequestReflectBody& body) noexcept;
/** @return True when the input is one complete message-57 body. */
[[nodiscard]] bool validate_perf_request_reflect(std::span<const std::byte> input) noexcept;

} // namespace sunrise::middleware::bap::activity_message::host_control
