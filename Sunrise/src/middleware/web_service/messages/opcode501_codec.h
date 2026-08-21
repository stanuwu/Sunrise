#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** Web Service opcode for the create-character request. */
inline constexpr std::uint16_t kOpcode = 501;

/** Exact native-width presentation block decoded from the observed creator request. */
inline constexpr std::size_t kPresentationHeaderSize = 36;
/** Exact native-width creation-header block decoded from the observed creator request. */
inline constexpr std::size_t kCreationHeaderSize = 36;
/** Exact native-width final creator block decoded from the observed creator request. */
inline constexpr std::size_t kCreationTailSize = 24;

/**
 * Stable identity plus the losslessly retained native creator blocks.
 *
 * The block contents deliberately remain opaque here. Their field-level semantics belong to the
 * native consumers that publish them, not to the request decoder.
 */
struct DecodedRequest {
    std::uint8_t race{};
    std::uint8_t gender{};
    std::uint8_t characterClass{};
    std::array<std::byte, kPresentationHeaderSize> presentationHeader{};
    std::array<std::byte, kCreationHeaderSize> creationHeader{};
    std::array<std::byte, kCreationTailSize> creationTail{};
    /** Opaque final five bits retained exactly; semantics are not yet assigned. */
    std::uint8_t creatorTrailer{};
};

/**
 * Decodes the observed native create-character request.
 *
 * The decoder validates the measured 97-byte layout, its fixed identity bits, and the supported
 * race/gender/class ranges before returning the native-width creator blocks.
 *
 * @param message Parsed opcode-501 request envelope.
 * @param request Receives the decoded identity and creator blocks.
 * @return True only when the request matches the verified native layout.
 */
[[nodiscard]] bool decode_request(const Message& message, DecodedRequest& request) noexcept;

/**
 * Encodes the create-character response: the status pair then the character object id.
 * @param message Parsed request whose envelope fields are echoed.
 * @param characterSoid Character object id; must be published in family 3.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when the fixed response fits the output buffer.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   std::uint64_t characterSoid,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
