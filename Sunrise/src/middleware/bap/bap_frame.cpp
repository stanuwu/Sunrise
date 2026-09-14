#include <algorithm>
#include <limits>

#include "../encoding/byte_order.h"
#include "frame.h"

namespace sunrise::middleware::bap {
namespace {

/** Protocol marker byte 1 starts every BAP outer frame, sent and received. */
constexpr std::byte kMagic{0x01};
/** Fixed wire offsets for the 6-byte BAP outer header. */
constexpr std::size_t kOuterMagicOffset = 0;
constexpr std::size_t kOuterTypeOffset = 1;
constexpr std::size_t kOuterLengthOffset = 2;
constexpr std::size_t kOuterHeaderSize = kOuterLengthOffset + encoding::kU32Size;
/** Fixed wire offsets for the 6-byte request and 8-byte response headers. */
constexpr std::size_t kServiceOffset = 0;
constexpr std::size_t kTaskOffset = kServiceOffset + encoding::kU16Size;
constexpr std::size_t kRequestHeaderSize = kTaskOffset + encoding::kU32Size;
constexpr std::size_t kStatusOffset = kRequestHeaderSize;
constexpr std::size_t kResponseHeaderSize = kStatusOffset + encoding::kU16Size;
/** The BAP response status the Client reads as success. */
constexpr std::uint16_t kStatusOk = 200;

/** @return True for either plaintext BAP frame-type value. */
[[nodiscard]] bool is_plaintext(FrameType frameType) noexcept {
    return frameType == FrameType::plaintext0 || frameType == FrameType::plaintext2;
}

} // namespace

/** Reads one whole BAP outer frame and borrows the payload it declares. */
bool parse_frame(std::span<const std::byte> input, OuterFrame& frame) noexcept {
    frame = {};
    if (input.size() < kOuterHeaderSize || input[kOuterMagicOffset] != kMagic) {
        return false;
    }
    const std::size_t payloadSize =
        encoding::read_u32_be(input.subspan<kOuterLengthOffset, encoding::kU32Size>());
    // Bytes past the declared payload are outside its authentication tag, so a frame carrying
    // them is not the frame the peer signed.
    if (payloadSize != input.size() - kOuterHeaderSize) {
        return false;
    }
    frame.frameType =
        static_cast<FrameType>(std::to_integer<std::uint8_t>(input[kOuterTypeOffset]));
    frame.payload = input.subspan(kOuterHeaderSize, payloadSize);
    return true;
}

/** Extracts one bounded frame from an arbitrary TCP prefix. */
StreamFrameResult parse_stream_frame(std::span<const std::byte> input,
                                     std::size_t maximumFrameSize,
                                     OuterFrame& frame,
                                     std::size_t& consumed) noexcept {
    frame = {};
    consumed = 0;
    if (maximumFrameSize < kOuterHeaderSize || (!input.empty() && input[0] != kMagic)) {
        return StreamFrameResult::invalid;
    }
    if (input.size() < kOuterHeaderSize) {
        return StreamFrameResult::incomplete;
    }
    const auto type =
        static_cast<FrameType>(std::to_integer<std::uint8_t>(input[kOuterTypeOffset]));
    const std::size_t payload =
        encoding::read_u32_be(input.subspan<kOuterLengthOffset, encoding::kU32Size>());
    if ((!is_plaintext(type) && type != FrameType::encrypted)
        || payload > maximumFrameSize - kOuterHeaderSize) {
        return StreamFrameResult::invalid;
    }
    const auto size = kOuterHeaderSize + payload;
    if (input.size() < size) {
        return StreamFrameResult::incomplete;
    }
    if (!parse_frame(input.first(size), frame)) {
        return StreamFrameResult::invalid;
    }
    consumed = size;
    return StreamFrameResult::complete;
}

/** Parses a decrypted or plaintext BAP request payload. */
bool parse_request_payload(std::span<const std::byte> input,
                           FrameType frameType,
                           RequestFrame& request) noexcept {
    request = {};
    if (input.size() < kRequestHeaderSize) {
        return false;
    }
    request.frameType = frameType;
    request.serviceId = encoding::read_u16_be(input.subspan<kServiceOffset, encoding::kU16Size>());
    request.taskId = encoding::read_u32_be(input.subspan<kTaskOffset, encoding::kU32Size>());
    request.body = input.subspan(kRequestHeaderSize);
    return true;
}

/** Parses a complete plaintext BAP request. */
bool parse_request(std::span<const std::byte> input, RequestFrame& request) noexcept {
    OuterFrame outer;
    if (!parse_frame(input, outer) || !is_plaintext(outer.frameType)) {
        request = {};
        return false;
    }
    return parse_request_payload(outer.payload, outer.frameType, request);
}

/** Reads the correlation and status without interpreting service-specific response bodies. */
bool parse_response_payload(std::span<const std::byte> input, ResponseFrame& response) noexcept {
    response = {};
    if (input.size() < kResponseHeaderSize) {
        return false;
    }
    response.serviceId = encoding::read_u16_be(input.subspan<kServiceOffset, encoding::kU16Size>());
    response.taskId = encoding::read_u32_be(input.subspan<kTaskOffset, encoding::kU32Size>());
    response.status = encoding::read_u16_be(input.subspan<kStatusOffset, encoding::kU16Size>());
    response.body = input.subspan(kResponseHeaderSize);
    return true;
}

bool encode_request_payload(RequestService service,
                            std::uint32_t taskId,
                            std::span<const std::byte> body,
                            std::span<std::byte> output,
                            std::size_t& written) noexcept {
    written = 0;
    if (body.size() > std::numeric_limits<std::uint32_t>::max() - kRequestHeaderSize
        || output.size() < kRequestHeaderSize + body.size()) {
        return false;
    }
    encoding::write_u16_be(output.subspan<kServiceOffset, encoding::kU16Size>(),
                           static_cast<std::uint16_t>(service));
    encoding::write_u32_be(output.subspan<kTaskOffset, encoding::kU32Size>(), taskId);
    std::copy(body.begin(), body.end(), output.begin() + kRequestHeaderSize);
    written = kRequestHeaderSize + body.size();
    return true;
}

/** Encodes one status-200 BAP response header and body. */
bool encode_response_payload(ResponseService service,
                             std::uint32_t taskId,
                             std::span<const std::byte> body,
                             std::span<std::byte> output,
                             std::size_t& written) noexcept {
    written = 0;
    if (body.size() > std::numeric_limits<std::uint32_t>::max() - kResponseHeaderSize
        || output.size() < kResponseHeaderSize + body.size()) {
        return false;
    }
    encoding::write_u16_be(output.subspan<kServiceOffset, encoding::kU16Size>(),
                           static_cast<std::uint16_t>(service));
    encoding::write_u32_be(output.subspan<kTaskOffset, encoding::kU32Size>(), taskId);
    encoding::write_u16_be(output.subspan<kStatusOffset, encoding::kU16Size>(), kStatusOk);
    std::copy(body.begin(), body.end(), output.begin() + kResponseHeaderSize);
    written = kResponseHeaderSize + body.size();
    return true;
}

/** Encodes a server notification with the smallest 6-byte inner header. */
bool encode_notification_payload(NotificationService service,
                                 std::uint32_t sequence,
                                 std::span<const std::byte> body,
                                 std::span<std::byte> output,
                                 std::size_t& written) noexcept {
    written = 0;
    if (body.size() > std::numeric_limits<std::uint32_t>::max() - kRequestHeaderSize
        || output.size() < kRequestHeaderSize + body.size()) {
        return false;
    }
    encoding::write_u16_be(output.subspan<kServiceOffset, encoding::kU16Size>(),
                           static_cast<std::uint16_t>(service));
    encoding::write_u32_be(output.subspan<kTaskOffset, encoding::kU32Size>(), sequence);
    std::copy(body.begin(), body.end(), output.begin() + kRequestHeaderSize);
    written = kRequestHeaderSize + body.size();
    return true;
}

/** Encodes one BAP outer header around an existing payload. */
bool encode_frame(FrameType frameType,
                  std::span<const std::byte> payload,
                  std::span<std::byte> output,
                  std::size_t& written) noexcept {
    written = 0;
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()
        || output.size() < kOuterHeaderSize + payload.size()) {
        return false;
    }
    output[kOuterMagicOffset] = kMagic;
    output[kOuterTypeOffset] = static_cast<std::byte>(frameType);
    encoding::write_u32_be(output.subspan<kOuterLengthOffset, encoding::kU32Size>(),
                           static_cast<std::uint32_t>(payload.size()));
    // Payload may already sit at its final offset inside output, so copy element by element.
    for (std::size_t index = 0; index < payload.size(); ++index) {
        output[kOuterHeaderSize + index] = payload[index];
    }
    written = kOuterHeaderSize + payload.size();
    return true;
}

/** Encodes one complete plaintext status-200 BAP response. */
bool encode_response(ResponseService service,
                     std::uint32_t taskId,
                     FrameType frameType,
                     std::span<const std::byte> body,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (!is_plaintext(frameType) || output.size() < kOuterHeaderSize) {
        return false;
    }
    std::size_t payloadSize = 0;
    if (!encode_response_payload(
            service, taskId, body, output.subspan(kOuterHeaderSize), payloadSize)) {
        return false;
    }
    return encode_frame(frameType, output.subspan(kOuterHeaderSize, payloadSize), output, written);
}

} // namespace sunrise::middleware::bap
