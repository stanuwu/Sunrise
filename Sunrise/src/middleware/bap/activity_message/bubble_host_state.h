#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../content/packages/schema/generated/tag_classes_808086.h"

namespace sunrise::middleware::bap::activity_message::bubble_host_state {

namespace generated = middleware::content::packages::schema::generated;

/** Message type and schema the client accepts for the bubble host table. */
inline constexpr std::uint32_t kMessageType = 54;
inline constexpr std::uint32_t kSchema = generated::C80808652::kClass;
/** The six-bit row count caps the table at 32 rows, and each row is fixed width. */
inline constexpr std::size_t kMaximumRows = 32;
inline constexpr std::size_t kSessionIdBytes = 128;
inline constexpr std::size_t kExternalAddressBytes = 86;
inline constexpr std::size_t kCountBits = 6;
inline constexpr std::size_t kRowBits = 1'829;
/** Body size the encoder buffer must cover at the full row count. */
inline constexpr std::size_t kMaximumBits = kCountBits + kMaximumRows * kRowBits;
inline constexpr std::size_t kMaximumBytes = (kMaximumBits + 7U) / 8U;

/** One complete fixed-width row in the message-54 host table. */
struct Row final {
    std::int8_t state{-1};
    std::int32_t sliceSetIndex{-1};
    std::int8_t peerIndex{-1};
    std::int32_t bubbleHostId{};
    std::array<std::int8_t, kSessionIdBytes> sessionId{};
    bool unresponsive{};
    std::array<std::byte, kExternalAddressBytes> externalAddress{};
    std::uint64_t unnamed{};
    bool ready{};
};

/** Complete replacement table carried by one message-54 notification. */
struct Table final {
    std::array<Row, kMaximumRows> rows{};
    std::uint8_t count{};
};

/** Computes the exact meaningful bit count for a bounded row count. */
[[nodiscard]] bool encoded_bit_count(std::size_t count, std::size_t& bits) noexcept;

/** Computes the exact byte extent, including zero padding in the last byte. */
[[nodiscard]] bool encoded_byte_count(std::size_t count, std::size_t& bytes) noexcept;

/** Encodes one complete replacement table in the client's MSB-first reflection order. */
[[nodiscard]] bool
encode(const Table& table, std::span<std::byte> output, std::size_t& written) noexcept;

/** Decodes one exact message-54 body and rejects nonzero trailing padding. */
[[nodiscard]] bool decode(std::span<const std::byte> input, Table& table) noexcept;

/** Checks one exact message-54 body without retaining its decoded state. */
[[nodiscard]] bool validate(std::span<const std::byte> input) noexcept;

} // namespace sunrise::middleware::bap::activity_message::bubble_host_state
