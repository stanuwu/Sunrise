#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace sunrise::middleware::web_service {

/** The 6-byte Web Service header holds a big-endian opcode and transaction id. */
inline constexpr std::size_t kEnvelopeHeaderSize = sizeof(std::uint16_t) + sizeof(std::uint32_t);
/** Every response ends with two cleared optional envelope fields. */
inline constexpr std::uint8_t kAbsentTrailerWidth = 2;

/** Parsed request header and borrowed bit-packed payload. */
struct Message {
    std::uint16_t opcode{};
    std::uint32_t transactionId{};
    std::span<const std::byte> payload{};
};

/** Reusable response payload layouts proven for stateless Web Service opcodes. */
enum class ResponseShape : std::uint8_t {
    statusOnly,
    statusPair,
    statusPairWithBool,
    // Header echo with an empty payload, for opcodes with no contract of their own.
    // Keeps the correlated task from hanging. The client decoder may under-run.
    generic,
};

/** Logical status values encoded with the protocol descriptor biases. */
struct StatusResponse {
    std::int32_t code{};
    std::int32_t value{(std::numeric_limits<std::int32_t>::min)()};
    bool trailingBool{};
};

/**
 * Parses the byte-aligned Web Service header and keeps the rest as payload.
 * @param input Whole decrypted svc-10 body.
 * @param message Receives the opcode, transaction id, and borrowed payload.
 * @return True when the 6-byte header is present.
 */
[[nodiscard]] bool parse_request(std::span<const std::byte> input, Message& message) noexcept;

/**
 * Encodes one supported bit-packed response body with cleared optional trailers.
 * @param request Parsed request whose opcode and transaction id are echoed.
 * @param shape Descriptor layout to encode.
 * @param status Logical status values before descriptor biases.
 * @param output Caller-owned svc-11 body storage.
 * @param written Receives encoded response body bytes.
 * @return True when the output buffer holds the whole response.
 */
[[nodiscard]] bool encode_response(const Message& request,
                                   ResponseShape shape,
                                   const StatusResponse& status,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service
